#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise the PCjr floppy adapter through its native non-DMA ports."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import encode, launch


# pylint: disable-next=R0913
def run_probe(dosbox, nasm, folder, image, replay, *, watchdog=False):
    """Run a direct FDC probe with DMA channel 2 masked."""
    options = ["-DREPLAY"] if replay else []
    if watchdog:
        options.append("-DWATCHDOG")
    if image.suffix == ".mfm":
        options.append("-DTRACK_IMAGE")
    subprocess.run([nasm, "-f", "bin", *options,
                    Path(__file__).with_name("pcjr_floppy_pio_probe.asm"),
                    "-o", folder / "PROBE.COM"], check=True)
    report = folder / "PIO.OUT"
    report.unlink(missing_ok=True)
    access = " -ro" if replay else ""
    commands = [f'mount c "{folder}"', f'mount a "{image}" -t floppy -fs none{access}']
    if image.suffix == ".mfm":
        commands.append(f'mount b "{folder / "other.mfm"}" -t floppy -fs none -ro')
    args, env = launch(dosbox, folder, "pio", [*commands, "c:", "probe.com", "exit"])
    config = args[-1]
    config.write_text("[dosbox]\nmachine=pcjr\n[cpu]\ncpu_cycles=3000\n"
                      + config.read_text(), encoding="ascii")
    result = subprocess.run(args, cwd=folder, env=env, capture_output=True, text=True,
                            check=True, timeout=30)
    stage = report.read_bytes() if report.exists() else b""
    if stage != b"\0":
        raise RuntimeError(f"{image.suffix} replay={replay}, stage={stage.hex()}\n"
                           f"{result.stdout}{result.stderr}")


def main():
    """Verify direct read, write, result status and persistence."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--watchdog", action="store_true")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        folder = Path(temp)
        disk = b"Q" * 512 + b"R" * 512 + bytes(512) + b"S" * 512 + bytes(360 * 1024 - 2048)
        if args.watchdog:
            image = folder / "disk.img"
            image.write_bytes(disk)
            run_probe(args.dosbox.resolve(), args.nasm, folder, image, False, watchdog=True)
            print("PCjr watchdog timeout, retrigger, cancellation, and recovery passed")
            return
        for extension, capture in (("img", disk), ("mfm", encode(disk, deleted_sector=True))):
            image = folder / f"disk.{extension}"
            image.write_bytes(capture)
            if extension == "mfm":
                (folder / "other.mfm").write_bytes(capture)
            for replay in (False, True):
                run_probe(args.dosbox.resolve(), args.nasm, folder, image, replay)
            if extension == "img":
                assert image.read_bytes() == b"\xa7" * 512 + b"\xb8" * 512 + disk[1024:]
            else:
                assert image.read_bytes() == capture
                assert image.with_suffix(".mfm.overlay").is_file()
            print(f".{extension}: PCjr non-DMA reads, writes, and restart passed")


if __name__ == "__main__":
    main()
