#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Preserve recorded SCP data across long intervals without transitions."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_read_deleted_probe import make_track
from run_floppy_zero_sector_probe import encode_tracks
from run_scp_index_continuity_probe import scp_image
from run_transcopy_layout_probe import run_guest


def track(marker):
    """Remove 32 recorded cells inside a payload while retaining its old CRC."""
    cells = make_track(((1, 0xfb, "", marker),), 2)
    first = cells.find(b"\x44\x89" * 3)
    data = cells.find(b"\x44\x89" * 3, first + 6) + 8
    cells[data + 32:data + 36] = bytes(4)
    payload = bytes([marker]) * 16 + bytes(2) + bytes([marker]) * 494
    return cells, payload


def check_capture(dosbox, nasm, directory, case):
    """Keep bad-CRC payloads equal to recorded samples, without inferred noise."""
    first, payload = track(0x77)
    second, other = track(0x55 if case == "varied" else 0x77)
    original = encode_tracks("scp", [first]) if case == "single" else scp_image(first + second)
    image = directory / f"{case}.scp"
    image.write_bytes(original)
    checks = """
    mov di,records
    mov bp,32
next_read:
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jnc done
    cmp ah,10h
    jne done
    mov si,buffer
    mov cx,512
    rep movsb
    dec bp
    jnz next_read
    mov dx,name
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc done
    mov bx,ax
    mov dx,records
    mov cx,16384
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
    jmp collected
name db 'READS.BIN',0
records times 16384 db 0
collected:
"""
    run_guest(dosbox, nasm, directory, image, checks)
    result = (directory / "READS.BIN").read_bytes()
    assert len(result) == 16384
    observed = {result[i:i + 512] for i in range(0, len(result), 512)}
    expected = {payload, other} if case == "varied" else {payload}
    assert observed == expected, (case, len(observed), len(expected))
    assert image.read_bytes() == original
    assert not Path(str(image) + ".overlay").exists()
    print(f"SCP {case}: recorded payloads preserved over 32 CRC-error reads", flush=True)


def main():
    """Check single, repeated and genuinely differing captured revolutions."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        for case in ("single", "repeated", "varied"):
            check_capture(args.dosbox.resolve(), args.nasm, Path(temp), case)


if __name__ == "__main__":
    main()
