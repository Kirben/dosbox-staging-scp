#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify 86F MOUNT, BOOT and controller access."""

import argparse
from pathlib import Path
from struct import pack, pack_into
import subprocess
from tempfile import TemporaryDirectory

from floppy_fixture_cache import cached_fixture

from run_hxc_mfm_probe import check_boot, check_fat, check_overlay_format, encode_track, launch
from run_raw_floppy_read_probe import run_probe


@cached_fixture
def encode_86f(disk, verify_errors=False, *, write_protected=False, physical_hole=False):
    """Encode the raw fixture as an 86Box track image."""
    image = bytearray(8 + 512 * 4)
    # Two-sided DD, surface arrays, explicit extra cells, little-endian words.
    flags = 0x0899 if write_protected else 0x0889
    pack_into("<4sHH", image, 0, b"86BF", 0x020c, flags)
    for track in range(80):
        cells = encode_track(disk, track, verify_errors=verify_errors).ljust(25_000, b"\0")
        little_endian = bytearray(len(cells))
        little_endian[0::2] = cells[1::2]
        little_endian[1::2] = cells[0::2]
        # Shorten the gap by three cells and place index between IDs 1 and 2.
        surface = bytearray(25_000)
        if physical_hole and track == 0:
            surface[0] = little_endian[0] ^ 0xff
        record = pack("<HiI", 0x0a, -3, 5001) + little_endian + surface
        cylinder, head = divmod(track, 2)
        for half in (0, 1):
            entry = 4 * cylinder + 2 * half + head
            pack_into("<I", image, 8 + 4 * entry, len(image))
            image.extend(record)
    return image


def check_invalid_length(dosbox, directory):
    """Reject an extra-cell count that would overflow signed 32-bit storage."""
    capture = bytearray(8 + 256 * 4)
    pack_into("<4sHHI", capture, 0, b"86BF", 0x020c, 0x80, len(capture))
    capture.extend(pack("<HiI", 0x0a, 0x7fffffff, 0))
    image = directory / "invalid-length.86f"
    image.write_bytes(capture)
    args, env = launch(dosbox, directory, "86f-invalid",
                       [f'mount a "{image}" -t floppy -fs none -ro', "exit"])
    run = subprocess.run(args, cwd=directory, env=env, timeout=30, check=True,
                         capture_output=True, text=True)
    assert "invalid 86F track length or index" in run.stdout + run.stderr


def main():
    """Run the 86F guest checks."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        check_invalid_length(dosbox, directory)
        run_probe(dosbox, args.nasm, directory, "86f", 0, 360, 9, encode_86f)
        check_fat(dosbox, directory, encode_86f, "86f")
        check_boot(dosbox, args.nasm, directory, encode_86f, "86f")
        check_overlay_format(dosbox, args.nasm, directory, "86f", encode_86f)


if __name__ == "__main__":
    main()
