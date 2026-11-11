#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify read-only TransCopy MOUNT, BOOT and controller reads."""

import argparse
from functools import partial
from pathlib import Path
from struct import pack_into
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import check_boot, check_fat, encode_track
from run_raw_floppy_read_probe import run_probe


def encode_transcopy(disk, *, step=1):
    """Build a native track table, with empty slots for skipped cylinders."""
    image = bytearray(0xa00)
    image[:2] = b"\x5a\xa5"
    image[0x100:0x105] = bytes((7, 0, 39, 2, step))
    for cylinder in range(0, 40, step):
        for head in range(2):
            track = 2 * cylinder + head
            cells = encode_track(disk, track)
            pack_into("<H", image, 0x105 + 2 * track, 10_000)
            pack_into(">H", image, 0x305 + 2 * track, len(image) // 256)
            pack_into("<H", image, 0x505 + 2 * track, len(cells))
            image.extend(cells)
            image.extend(bytes(-len(image) % 256))
    return image


def main():
    """Exercise the DOS, BIOS and FDC entry points."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        run_probe(dosbox, args.nasm, directory, "tc", 0, 360, 9,
                  partial(encode_transcopy, step=2))
        check_fat(dosbox, directory, encode_transcopy, "tc", writable=False)
        check_boot(dosbox, args.nasm, directory, encode_transcopy, "tc", writable=False)


if __name__ == "__main__":
    main()
