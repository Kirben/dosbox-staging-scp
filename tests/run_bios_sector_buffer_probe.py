#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check BIOS sector buffers for raw fixed disks and variable-size track images."""

import argparse
from pathlib import Path
from struct import pack
import subprocess
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_hxc_mfm_probe import encode, launch
from run_scp_attachment_probe import encode_scp


# pylint: disable-next=R0913,R0917
def run_probe(dosbox, nasm, directory, mount, options, machine="svga_s3"):
    """Run one BIOS guest probe and read its result marker."""
    source = Path(__file__).with_name("bios_sector_buffer_probe.asm")
    subprocess.run([nasm, "-f", "bin", *options, source, "-o", directory / "PROBE.COM"],
                   check=True)
    report = directory / "SIZE.OUT"
    report.unlink(missing_ok=True)
    args, env = launch(dosbox, directory, "sector-buffer",
                       [f'mount c "{directory}"', mount, "c:", "probe.com", "exit"])
    config = args[-1]
    config.write_text(f"[dosbox]\nmachine={machine}\n" + config.read_text(),
                      encoding="ascii")
    run = subprocess.run(args, env=env, cwd=directory, check=True, capture_output=True,
                         text=True, timeout=30)
    result = report.read_bytes() if report.exists() else b""
    if result != b"\0":
        raise RuntimeError(f"{machine} {options}: stage {result.hex()}\n{run.stdout}{run.stderr}")


def legacy_overlay(capture):
    """Build a version 4 sidecar with one logical and one physical record."""
    fingerprint = 14695981039346656037
    for byte in capture:
        fingerprint = ((fingerprint ^ byte) * 1099511628211) & ((1 << 64) - 1)
    formatted = bytes((0, 0xff, 0, 2, 0xe5, 0, 0, 1, 2, 0, 0, 9, 2))
    return (b"SCPOVL\x04\0" + pack("<Q", fingerprint) + formatted +
            bytes((0, 0, 9)) + b"Y" * 512 +
            bytes((0, 0xfd, 0)) + pack("<I", 1) + b"s" * 512)


def check_overlay_writes(dosbox, nasm, directory):
    """Write and remount variable-size sectors alongside existing overlay data."""
    for extension, encoder in (("scp", encode_scp), ("mfm", encode), ("86f", encode_86f)):
        image = directory / f"disk.{extension}"
        capture = encoder(bytes(360 * 1024))
        image.write_bytes(capture)
        overlay = image.with_suffix(f".{extension}.overlay")
        old_overlay = legacy_overlay(capture)
        mount = f'mount a "{image}" -t floppy -fs none'
        for machine in ("svga_s3", "pcjr", "tandy"):
            overlay.write_bytes(old_overlay)
            for size_code, cylinder in ((1, 7), (3, 8)):
                options = [f"-DSIZE_CODE={size_code}", f"-DSECTOR_BYTES={128 << size_code}",
                           f"-DCYLINDER={cylinder}", "-DOVERLAY_WRITE"]
                if machine == "pcjr":
                    options.append("-DSECTOR_PATH")
                run_probe(dosbox, nasm, directory, mount, options, machine)
            saved = overlay.read_bytes()
            # These writes first format tracks at an explicit data rate.
            assert saved[6] == 8
            for size_code, cylinder in ((1, 7), (3, 8)):
                options = [f"-DSIZE_CODE={size_code}", f"-DSECTOR_BYTES={128 << size_code}",
                           f"-DCYLINDER={cylinder}", "-DOVERLAY_REPLAY"]
                run_probe(dosbox, nasm, directory, mount + " -ro", options, machine)
            assert overlay.read_bytes() == saved
            assert image.read_bytes() == capture
            print(f".{extension} {machine}: "
                  "256- and 1024-byte writes and legacy overlay replay passed")


def main():
    """Test raw fixed-disk I/O and floppy format, read, and verify."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--overlay-write", action="store_true")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        if args.overlay_write:
            check_overlay_writes(dosbox, args.nasm, directory)
            return
        disk = directory / "fixed.img"
        disk.write_bytes(b"1" * 256 + b"2" * 256 + bytes(256 * 254))
        mount = f'mount 2 "{disk}" -t hdd -fs none -size 256,16,4,4'
        run_probe(dosbox, args.nasm, directory, mount, ["-DFIXED_DISK"])
        assert disk.read_bytes() == b"w" * 512 + bytes(256 * 254)
        print("256-byte raw fixed-disk reads and writes passed")

        image = directory / "disk.mfm"
        capture = encode(bytes(360 * 1024))
        image.write_bytes(capture)
        mount = f'mount a "{image}" -t floppy -fs none'
        overlay = image.with_suffix(".mfm.overlay")
        for machine in ("svga_s3", "pcjr", "tandy"):
            for size_code in (1, 3):
                overlay.unlink(missing_ok=True)
                size = 128 << size_code
                run_probe(dosbox, args.nasm, directory, mount,
                          [f"-DSIZE_CODE={size_code}", f"-DSECTOR_BYTES={size}"], machine)
                print(f"{machine}: {size}-byte floppy format, read and verify passed")
        assert image.read_bytes() == capture


if __name__ == "__main__":
    main()
