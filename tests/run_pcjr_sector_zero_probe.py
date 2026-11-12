#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check BIOS sector-zero access in PCjr/Tandy, with a standard-PC control."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_zero_sector_probe import encode_image
from run_transcopy_layout_probe import run_guest


SETUP = """
    mov ax,351eh
    int 21h
    mov byte [es:bx+3],1
    push cs
    pop es
"""

READ_ZERO = """
    mov ax,0201h
    mov bx,buffer
    mov cx,0100h
    xor dx,dx
    int 13h
    jc done
"""


def check_zero(dosbox, nasm, directory, extension, machine):
    """Read, verify, write and reopen a valid sector-zero field."""
    image = directory / f"{machine}.{extension}"
    original = encode_image(extension, True)
    image.write_bytes(original)
    checks = SETUP + """
    mov ax,0201h
    mov bx,buffer
    mov cx,0101h
    xor dx,dx
    int 13h
    jc done
    cmp byte [buffer],71h
    jne done
    inc byte [stage]
""" + READ_ZERO + """
    cmp byte [buffer],70h
    jne done
    inc byte [stage]
    mov ax,0401h
    mov bx,buffer
    mov cx,0100h
    xor dx,dx
    int 13h
    jc done
    cmp byte [buffer],70h
    jne done
    inc byte [stage]
    mov di,buffer
    mov cx,256
    mov al,66h
    rep stosb
    mov ax,0301h
    mov bx,buffer
    mov cx,0100h
    xor dx,dx
    int 13h
    jc done
"""
    run_guest(dosbox, nasm, directory, image, checks, machine=machine, read_only=False)
    checks = SETUP + READ_ZERO + """
    mov si,buffer
    mov cx,256
check_payload:
    lodsb
    cmp al,66h
    jne done
    loop check_payload
"""
    run_guest(dosbox, nasm, directory, image, checks, machine=machine)
    assert image.read_bytes() == original


def main():
    """Use identical physical sectors in four containers and three machines."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        for extension in ("tc", "mfm", "scp", "86f"):
            for machine in ("svga_s3", "pcjr", "tandy"):
                check_zero(args.dosbox.resolve(), args.nasm, Path(temp),
                           extension, machine)
                print(f".{extension} / {machine}: BIOS sector zero and restart passed",
                      flush=True)


if __name__ == "__main__":
    main()
