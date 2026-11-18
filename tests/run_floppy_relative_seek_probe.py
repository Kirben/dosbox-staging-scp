#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test Relative Seek motion, PCN wrapping, completion IRQ and TRACK0."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_id_search_probe import wait_microseconds
from run_floppy_recalibrate_limit_probe import physical_track, send, sense
from run_floppy_transfer_edges_probe import RESET, program
from run_floppy_zero_sector_probe import encode_tracks, encoded_track
from run_transcopy_layout_probe import run_guest


def complete(status, pcn, delay=20000):
    """Wait for IRQ6 and consume the seek result."""
    return wait_microseconds(delay) + """
    push ds
    xor ax,ax
    mov ds,ax
    test byte [043eh],80h
    pop ds
    jz done
""" + sense(status, pcn)


def checks(extended):
    """Exercise both directions and distinguish PCN from the physical track."""
    body = RESET + """
    mov dx,3f7h
    mov al,2
    out dx,al
""" + send("specify", 3) + send("inward", 3) + """
    mov dx,3f4h
    in al,dx
    cmp al,81h
    jne done
""" + send("other_drive", 3) + """
    in al,dx
    cmp al,80h
    jne done
""" + complete(0x20, 3) + physical_track(3)
    body += send("outward", 3) + complete(0x20, 1) + physical_track(1)
    body += send("outward", 3) + complete(0x70, 0) + physical_track(0)
    body += send("outward", 3) + complete(0x70, 0) + physical_track(0)
    commands = """
specify db 3,0ffh,2
inward db 0cfh,0,3
outward db 8fh,0,2
other_drive db 0cfh,1,1
"""
    if extended:
        body += send("full_in", 3) + complete(0x20, 0, 540000)
        body += send("inward", 3) + complete(0x20, 3)
        body += send("full_out", 3) + complete(0x20, 3, 540000)
        body += physical_track(3)
        commands += "full_in db 0cfh,0,0\nfull_out db 8fh,0,0\n"
    return program(body, commands)


def main():
    """Check mechanical and interrupt behavior with every floppy backend."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    formats = ("img", "scp") if args.smoke else ("img", "ima", "scp", "mfm", "86f", "tc")
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for extension in formats:
            image = directory / f"relative.{extension}"
            original = bytes(360 * 1024) if extension in ("img", "ima") else encode_tracks(
                extension, [encoded_track(cylinder, False) for cylinder in range(4)])
            image.write_bytes(original)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      checks(extended=extension == "img"))
            assert image.read_bytes() == original
    print("Floppy Relative Seek regressions passed", flush=True)


if __name__ == "__main__":
    main()
