#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check that resetting the FDC leaves the physical head in place."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_zero_sector_probe import encode_image
from run_transcopy_layout_probe import run_guest


CHECKS = """
    push es
    xor ax,ax
    mov es,ax
    les bx,[es:78h]
    mov byte [es:bx+3],1
    pop es
    mov ax,0201h
    mov bx,buffer
    mov cx,0101h
    xor dx,dx
    int 13h
    jc done
    call read_id
    cmp byte [ids+3],1
    jne done
    inc byte [stage]
    mov dx,3f2h
    mov al,18h
    out dx,al
    mov al,1ch
    out dx,al
    mov dx,3f5h
    mov cx,4
reset_status:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop reset_status
    call read_id
    cmp byte [ids+3],1
    jne done
    jmp checked
read_id:
    mov dx,3f5h
    mov al,4ah
    out dx,al
    xor al,al
    out dx,al
    mov cx,50
wait_tick:
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
    je result
    loop wait_tick
    jmp done
result:
    mov dx,3f5h
    mov di,ids
    mov cx,7
read_byte:
    in al,dx
    stosb
    loop read_byte
    ret
ids times 7 db 0
checked:
"""

RAW_SEEK = """
    ; Raw BIOS reads retain their logical sector path; seek the FDC explicitly.
    mov dx,3f2h
    mov al,1ch
    out dx,al
    mov dx,3f5h
    mov cx,4
drain_initial_reset:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop drain_initial_reset
    mov al,0fh
    out dx,al
    xor al,al
    out dx,al
    mov al,1
    out dx,al
    mov ah,86h
    xor cx,cx
    mov dx,10000
    int 15h
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,20h
    jne done
    in al,dx
    cmp al,1
    jne done
"""


def main():
    """Check Read ID across DOR reset, with raw images as controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("tc", "mfm", "scp", "86f", "img", "ima"):
            image = directory / f"reset.{extension}"
            raw = extension in ("img", "ima")
            original = bytes(360 * 1024) if raw else encode_image(extension, False)
            image.write_bytes(original)
            checks = CHECKS.replace("[es:bx+3],1", "[es:bx+3],2") if raw else CHECKS
            if raw:
                checks = checks.replace("    call read_id", RAW_SEEK + "    call read_id", 1)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image, checks)
            assert image.read_bytes() == original
            print(f".{extension}: DOR reset preserves cylinder one", flush=True)


if __name__ == "__main__":
    main()
