#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify TransCopy reads, weak bits, and persistent overlay writes."""

import argparse
from functools import partial
from pathlib import Path
from struct import pack_into
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import check_boot, check_fat, check_overlay_format, encode_track
from run_raw_floppy_read_probe import run_probe


def encode_transcopy(disk, *, step=1, heads=2, weak_bits=False):
    """Build a native track table, with empty slots for skipped cylinders."""
    image = bytearray(0xa00)
    image[:2] = b"\x5a\xa5"
    image[0x100:0x105] = bytes((7, 0, 39, heads, step))
    for cylinder in range(0, 40, step):
        for head in range(heads):
            track = 2 * cylinder + head
            cells = encode_track(disk, track)
            if weak_bits and track == 5:
                # Sector 6 is the sixth ID/data pair on this track.
                sync = b"\x44\x89" * 3
                mark = cells.split(sync, 12)
                mark[12][2:34] = bytes(32)
                cells = sync.join(mark)
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
        for weak_bits in (False, True):
            encoder = partial(encode_transcopy, step=2, weak_bits=weak_bits)
            run_probe(dosbox, args.nasm, directory, "tc", 0, 360, 9, encoder,
                      weak_bits=weak_bits)
        print(".tc fixed and weak data passed repeated Read Data and Read Track")
        encoder = partial(encode_transcopy, step=2, weak_bits=True)
        overlay = directory / "disk.tc.overlay"
        for unit in (0, 1):
            overlay.unlink(missing_ok=True)
            run_probe(dosbox, args.nasm, directory, "tc", unit, 360, 9, encoder,
                      write_mode="protected")
            assert not overlay.exists()
            run_probe(dosbox, args.nasm, directory, "tc", unit, 360, 9, encoder,
                      write_mode="write")
            saved = overlay.read_bytes()
            for _ in range(2):
                run_probe(dosbox, args.nasm, directory, "tc", unit, 360, 9, encoder,
                          write_mode="readback", weak_bits=False)
                assert overlay.read_bytes() == saved
        print(".tc: writes replace weak data and survive repeated restarts on A and B")
        check_fat(dosbox, directory, encode_transcopy, "tc")
        check_boot(dosbox, args.nasm, directory, encode_transcopy, "tc")
        overlay.unlink(missing_ok=True)
        check_overlay_format(dosbox, args.nasm, directory, "tc", encoder)


if __name__ == "__main__":
    main()
