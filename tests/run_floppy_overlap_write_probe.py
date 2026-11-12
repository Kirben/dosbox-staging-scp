#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check that overlay writes invalidate overwritten embedded sector IDs."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import encode_field_fixture, run_guest


CHECKS = """
    push es
    xor ax,ax
    mov es,ax
    les bx,[es:78h]
    mov byte [es:bx+3],3
    pop es
    mov di,buffer
    mov cx,1024
    mov al,0a5h
    rep stosb
    mov ax,0301h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    inc byte [stage]
"""

INNER_WRITE = """
    push es
    xor ax,ax
    mov es,ax
    les bx,[es:78h]
    mov byte [es:bx+3],2
    pop es
    mov di,buffer
    mov cx,512
    mov al,66h
    rep stosb
    mov ax,0301h
    mov bx,buffer
    mov cx,0bbh
    xor dx,dx
    int 13h
    jc done
    inc byte [stage]
"""

READBACK = """
    push es
    xor ax,ax
    mov es,ax
    les bx,[es:78h]
    mov byte [es:bx+3],3
    pop es
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    cmp ax,1
    jne done
    mov si,buffer
    mov cx,1024
compare_outer:
    lodsb
    cmp al,0a5h
    jne done
    loop compare_outer
    inc byte [stage]
    push es
    xor ax,ax
    mov es,ax
    les bx,[es:78h]
    mov byte [es:bx+3],2
    pop es
    mov ax,0201h
    mov bx,buffer
    mov cx,0bbh
    xor dx,dx
    int 13h
    jnc done
    cmp ax,0400h
    jne done
    inc byte [stage]
    mov dx,3f5h
    mov al,4ah
    out dx,al
    xor al,al
    out dx,al
    mov cx,50
wait_id:
    push cx
    mov ah,86h
    xor cx,cx
    mov dx,10000
    int 15h
    mov dx,3f4h
    in al,dx
    and al,0d0h
    cmp al,0d0h
    pop cx
    je id_ready
    loop wait_id
    jmp done
id_ready:
    mov dx,3f5h
    mov di,buffer
    mov cx,7
read_id:
    in al,dx
    stosb
    loop read_id
    cmp word [buffer+3],0
    jne done
    cmp word [buffer+5],0301h
    jne done
"""

PARTIAL_FORMAT = """
    mov dx,3f2h
    mov al,1ch
    out dx,al
    mov dx,3f5h
    mov cx,4
drain_format_reset:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop drain_format_reset
    mov dword [buffer],02090000h
    mov al,6
    out 0ah,al
    xor al,al
    out 0ch,al
    xor eax,eax
    mov ax,ds
    shl eax,4
    add eax,buffer
    out 04h,al
    shr eax,8
    out 04h,al
    shr eax,8
    out 81h,al
    mov al,3
    out 05h,al
    xor al,al
    out 05h,al
    mov al,4ah
    out 0bh,al
    mov al,2
    out 0ah,al
    mov si,format_command
    mov cx,6
    mov dx,3f5h
send_format:
    lodsb
    out dx,al
    loop send_format
    mov cx,50
wait_format:
    push cx
    mov ah,86h
    xor cx,cx
    mov dx,10000
    int 15h
    mov dx,3f4h
    in al,dx
    and al,0d0h
    cmp al,0d0h
    pop cx
    je format_ready
    loop wait_format
    mov byte [stage],0eeh
    jmp done
format_ready:
    mov di,buffer
    mov dx,3f5h
    mov cx,7
format_result:
    in al,dx
    stosb
    loop format_result
    mov al,[buffer+1]
    or al,80h
    mov [stage],al
    cmp byte [buffer+1],10h
    jne done
    mov byte [stage],20
    jmp formatted
format_command db 4dh,0,2,255,54h,0e5h
formatted:
"""


def main():
    """Check immediate readback and read-only replay in fresh processes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    fixture, _ = encode_field_fixture(False)
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("tc", "mfm", "scp", "86f"):
            image = directory / f"overlap.{extension}"
            original = encode_tracks(extension, [fixture[0xa00:]])
            image.write_bytes(original)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      INNER_WRITE + CHECKS + READBACK, read_only=False)
            overlay = image.with_suffix(f".{extension}.overlay")
            saved = overlay.read_bytes()
            run_guest(args.dosbox.resolve(), args.nasm, directory, image, READBACK)
            assert overlay.read_bytes() == saved
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      (CHECKS + READBACK).replace("0a5h", "05ah"), read_only=False)
            saved = overlay.read_bytes()
            assert saved[6] == 6
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      READBACK.replace("0a5h", "05ah"))
            assert overlay.read_bytes() == saved
            surviving = READBACK.replace("0a5h", "05ah").replace(
                "cmp word [buffer+5],0301h\n    jne done",
                "cmp byte [buffer+5],0bbh\n    je done")
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      PARTIAL_FORMAT + surviving, read_only=False)
            saved = overlay.read_bytes()
            run_guest(args.dosbox.resolve(), args.nasm, directory, image, surviving)
            assert overlay.read_bytes() == saved
            assert image.read_bytes() == original
            print(f".{extension}: overwritten embedded ID stays absent after restart", flush=True)


if __name__ == "__main__":
    main()
