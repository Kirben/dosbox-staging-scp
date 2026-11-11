#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify SCP mounts, recorded revolutions, and persistent overlay writes."""

import argparse
from pathlib import Path
import re
from struct import pack, pack_into
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import check_boot, check_fat, encode_track
from run_raw_floppy_read_probe import run_probe


BIT_STRINGS = tuple(f"{value:08b}" for value in range(256))
TRANSITION = re.compile("1")


def encode_scp(disk, different_turns=False):
    image = bytearray(0x2b0)
    image[:3] = b"SCP"
    image[3:9] = bytes((0x10, 0x31, 2, 0, 79, 1))
    for track in range(80):
        turns = []
        last_edge = 0
        elapsed = 0
        for turn in range(2):
            if different_turns and track == 5:
                turn_disk = bytearray(disk)
                start = (track * 9 + 5) * 512
                turn_disk[start:start + 512] = bytes((0x61 + turn,)) * 512
            else:
                turn_disk = disk
            cells = encode_track(turn_disk, track)
            bits = "".join(BIT_STRINGS[value] for value in cells)
            flux = bytearray()
            for match in TRANSITION.finditer(bits):
                edge = elapsed + match.end()
                flux.extend(pack(">H", (edge - last_edge) * 80))
                last_edge = edge
            elapsed += len(bits)
            turns.append((len(bits) * 80, flux))

        pack_into("<I", image, 16 + 4 * track, len(image))
        image.extend(pack("<3sB", b"TRK", track))
        relative = 4 + 12 * len(turns)
        for duration, flux in turns:
            image.extend(pack("<III", duration, len(flux) // 2, relative))
            relative += len(flux)
        for _, flux in turns:
            image.extend(flux)
    pack_into("<I", image, 12, sum(image[16:]) & 0xffffffff)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        run_probe(dosbox, args.nasm, directory, "scp", 0, 360, 9,
                  lambda disk: encode_scp(disk, different_turns=True))
        print(".scp successive reads returned different recorded revolutions")
        for unit in (0, 1):
            overlay = directory / "disk.scp.overlay"
            overlay.unlink(missing_ok=True)
            for mode in ("protected", "write", "readback"):
                run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                          encode_scp, write_mode=mode)
                if mode == "protected":
                    assert not overlay.exists()
                elif mode == "write":
                    saved = overlay.read_bytes()
                else:
                    assert overlay.read_bytes() == saved
            for mode in ("format-protected", "format-invalid", "format",
                         "format-readback"):
                run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                          encode_scp, write_mode=mode)
                if mode == "format":
                    formatted = overlay.read_bytes()
                    assert formatted != saved
                elif mode == "format-readback":
                    assert overlay.read_bytes() == formatted
                else:
                    assert overlay.read_bytes() == saved
        check_fat(dosbox, directory, encode_scp, "scp")
        check_boot(dosbox, args.nasm, directory, encode_scp, "scp")


if __name__ == "__main__":
    main()
