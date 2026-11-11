#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check BIOS sector buffers for raw fixed disks and variable-size track images."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import encode, launch


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


def main():
    """Test raw fixed-disk I/O and floppy format, read, and verify."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
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
