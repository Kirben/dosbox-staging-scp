#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify read-only 86F MOUNT, BOOT and controller reads."""

import argparse
from pathlib import Path
from struct import pack, pack_into
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import check_boot, check_fat, encode_track
from run_raw_floppy_read_probe import run_probe


def encode_86f(disk, verify_errors=False):
    image = bytearray(8 + 512 * 4)
    # Two-sided DD, surface arrays, explicit extra cells, little-endian words.
    pack_into("<4sHH", image, 0, b"86BF", 0x020c, 0x0889)
    for track in range(80):
        cells = encode_track(disk, track, verify_errors=verify_errors).ljust(25_000, b"\0")
        little_endian = bytearray(len(cells))
        little_endian[0::2] = cells[1::2]
        little_endian[1::2] = cells[0::2]
        # Shorten the gap by three cells and place index between IDs 1 and 2.
        record = pack("<HiI", 0x0a, -3, 5001) + little_endian + bytes(25_000)
        cylinder, head = divmod(track, 2)
        for half in (0, 1):
            entry = 4 * cylinder + 2 * half + head
            pack_into("<I", image, 8 + 4 * entry, len(image))
            image.extend(record)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        run_probe(dosbox, args.nasm, directory, "86f", 0, 360, 9, encode_86f)
        check_fat(dosbox, directory, encode_86f, "86f")
        check_boot(dosbox, args.nasm, directory, encode_86f, "86f")


if __name__ == "__main__":
    main()
