#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify BIOS floppy reads, DMA page boundaries, and IRQ6 in a DOS guest."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_hxc_mfm_probe import encode, launch
from run_scp_attachment_probe import encode_scp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()

    disk = bytearray(360 * 1024)
    for sector in (3, 4):
        offset = (5 * 9 + sector - 1) * 512
        disk[offset:offset + 512] = bytes([0x50 + sector]) * 512

    source = Path(__file__).with_name("bios_floppy_read_probe.asm")
    formats = {"img": None, "ima": None, "scp": encode_scp,
               "mfm": encode, "86f": encode_86f}
    for extension, encoder in formats.items():
        image_bytes = encoder(disk) if encoder else disk
        for unit, letter in enumerate("ab"):
            with TemporaryDirectory() as temp:
                directory = Path(temp)
                image = directory / f"disk.{extension}"
                image.write_bytes(image_bytes)
                options = ["-DTRACK_IMAGE"] if encoder else []
                subprocess.run(
                    [args.nasm, "-f", "bin", f"-DUNIT={unit}", *options, source,
                     "-o", directory / "PROBE.COM"],
                    check=True,
                )
                command, env = launch(dosbox, directory, "read", [
                    f'mount c "{directory}"',
                    f'mount {letter} "{image}" -t floppy -fs none -ro',
                    "c:", "probe.com", "exit",
                ])
                run = subprocess.run(command, cwd=directory, env=env, timeout=30,
                                     capture_output=True, text=True, check=True)
                report = directory / "BIOS.OUT"
                stage = report.read_bytes() if report.exists() else b""
                if stage != b"\0":
                    raise RuntimeError(f".{extension} {letter}: stage {stage.hex()}\n"
                                       f"{run.stdout}{run.stderr}")
                mode = "DMA boundary and IRQ6" if encoder else "legacy sector path"
                print(f".{extension} drive {letter.upper()}: data and {mode} passed")


if __name__ == "__main__":
    main()
