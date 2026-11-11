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


# pylint: disable-next=R0914
def encode_scp(disk, different_turns=False, duplicate_id=False, verify_errors=False):
    """Encode the raw fixture as a multi-revolution SCP capture."""
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
            cells = encode_track(turn_disk, track, duplicate_id, verify_errors)
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


# pylint: disable-next=R0914,R0912,R0915
def main():
    """Run SCP attachment, write, and format scenarios."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--bios-format", action="store_true")
    parser.add_argument("--format-switch", action="store_true")
    parser.add_argument("--read-revolutions", action="store_true")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        if args.format_switch:
            for unit in (0, 1):
                overlay = directory / "disk.scp.overlay"
                overlay.unlink(missing_ok=True)
                run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                          encode_scp, write_mode="write")
                before = overlay.read_bytes()
                run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                          encode_scp, write_mode="format", partial_format=True,
                          format_switch=True)
                saved = overlay.read_bytes()
                assert saved != before
                run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                          encode_scp, write_mode="format-readback", partial_format=True)
                assert overlay.read_bytes() == saved
            return
        if args.bios_format:
            for unit in (0, 1):
                overlay = directory / "disk.scp.overlay"
                overlay.unlink(missing_ok=True)
                for mode in ("format-protected", "format-invalid", "format", "format-readback"):
                    run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                              encode_scp, write_mode=mode, bios_format=True)
                    if mode in ("format-protected", "format-invalid"):
                        assert not overlay.exists()
                    else:
                        assert overlay.is_file()
            return
        run_probe(dosbox, args.nasm, directory, "scp", 0, 360, 9,
                  lambda disk: encode_scp(disk, different_turns=True))
        print(".scp Read Data and Read Track returned different recorded revolutions")
        if args.read_revolutions:
            return
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
            run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                      encode_scp, write_mode="format", partial_format=True)
            interrupted = overlay.read_bytes()
            assert interrupted != saved
            run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                      encode_scp, write_mode="format-readback", partial_format=True)
            assert overlay.read_bytes() == interrupted
            overlay.write_bytes(saved)
            for interruption in ("format_reset", "format_motor"):
                for phase, expected in ((1, saved), (2, interrupted)):
                    run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                              encode_scp, write_mode="format", partial_format=True,
                              **{interruption: phase})
                    assert overlay.read_bytes() == expected
                run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                          encode_scp, write_mode="format-readback", partial_format=True)
                assert overlay.read_bytes() == interrupted
                overlay.write_bytes(saved)
            for mode in ("format-protected", "format-invalid", "format",
                         "format-readback"):
                run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                          encode_scp, write_mode=mode,
                          format_reset=3 if mode == "format" else None)
                if mode == "format":
                    formatted = overlay.read_bytes()
                    assert formatted != saved
                elif mode == "format-readback":
                    assert overlay.read_bytes() == formatted
                else:
                    assert overlay.read_bytes() == saved
            overlay.write_bytes(saved)
            run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                      encode_scp, write_mode="format", format_motor=3)
            assert overlay.read_bytes() == formatted
            run_probe(dosbox, args.nasm, directory, "scp", unit, 360, 9,
                      encode_scp, write_mode="format-readback")
        check_fat(dosbox, directory, encode_scp, "scp")
        check_boot(dosbox, args.nasm, directory, encode_scp, "scp")
        with TemporaryDirectory(dir=directory) as duplicate_temp:
            duplicate_dir = Path(duplicate_temp)
            overlay = duplicate_dir / "disk.scp.overlay"
            for unit in (0, 1):
                overlay.unlink(missing_ok=True)
                for mode in ("duplicate-write", "duplicate-readback"):
                    run_probe(dosbox, args.nasm, duplicate_dir, "scp", unit, 360, 9,
                              lambda disk: encode_scp(disk, duplicate_id=True),
                              write_mode=mode)
                    if mode == "duplicate-write":
                        duplicate_overlay = overlay.read_bytes()
                    else:
                        assert overlay.read_bytes() == duplicate_overlay


if __name__ == "__main__":
    main()
