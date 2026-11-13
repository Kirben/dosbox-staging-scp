#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check BIOS and controller disk-change reporting in a single guest session."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_transcopy_attachment_probe import encode_transcopy
from run_hxc_mfm_probe import encode, launch
from run_scp_attachment_probe import encode_scp


# pylint: disable-next=R0913,R0917
def check_changes(dosbox, nasm, directory, first, second, unit):
    """Check disk change reporting while replacing and ejecting a disk."""
    source = Path(__file__).with_name("floppy_disk_change_probe.asm")
    subprocess.run(
        [nasm, "-f", "bin", f"-DUNIT={unit}", source, "-o", directory / "PROBE.COM"],
        check=True,
    )
    letter = "ab"[unit]
    commands = [
        f'mount c "{directory}"', "c:", "probe 0",
        f'mount {letter} "{first}" -t floppy -fs none -ro', "probe 1",
        f'mount {letter} "{second}" -t floppy -fs none -ro', "probe 2",
        f"mount -u {letter}", "probe 3",
        f'mount {letter} "{first}" -t floppy -fs none -ro', "probe 4",
        "exit",
    ]
    command, env = launch(dosbox, directory, "change", commands)
    run = subprocess.run(command, cwd=directory, env=env, timeout=40,
                         capture_output=True, text=True, check=True)
    for stage in range(5):
        report = directory / f"CHANGE{stage}.OUT"
        if not report.exists() or report.read_bytes() != b"\0":
            raise RuntimeError(f"{first.suffix} drive {letter}, phase {stage} failed\n"
                               f"{run.stdout}{run.stderr}")
        report.unlink()
    print(f"{first.suffix} drive {letter.upper()}: insert, swap, eject and reinsert passed")


def main():
    """Run disk change checks for the supported floppy formats."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    for extension, encoder in (("img", bytes), ("ima", bytes), ("scp", encode_scp),
                               ("mfm", encode), ("86f", encode_86f),
                               ("tc", encode_transcopy)):
        with TemporaryDirectory() as temp:
            directory = Path(temp)
            first = directory / f"first.{extension}"
            second = directory / f"second.{extension}"
            disk = bytearray(360 * 1024)
            disk[0] = 0x31
            first.write_bytes(encoder(disk))
            disk[0] = 0x52
            second.write_bytes(encoder(disk))
            for unit in range(2):
                check_changes(dosbox, args.nasm, directory, first, second, unit)


if __name__ == "__main__":
    main()
