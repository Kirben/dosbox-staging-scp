#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check restoration of the IBM PC ROM diskette parameter table."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_hxc_mfm_probe import encode
from run_scp_attachment_probe import encode_scp
from run_transcopy_attachment_probe import encode_transcopy
from run_transcopy_layout_probe import run_guest


CHECKS = """
    ; Preserve the normal writable table while temporarily replacing INT 1Eh.
    push ds
    xor ax,ax
    mov ds,ax
    lds si,[78h]
    mov di,table
    mov cx,11
    cld
    rep movsb
    pop ds
    mov byte [table+4],8
    push es
    xor ax,ax
    mov es,ax
    mov word [es:78h],table
    mov ax,cs
    mov [es:7ah],ax
    pop es
    call read_sector
    jc done
    inc byte [stage]
    ; Stargate restores this address explicitly instead of saving INT 1Eh.
    push es
    xor ax,ax
    mov es,ax
    mov word [es:78h],0efc7h
    mov word [es:7ah],0f000h
    mov ax,0f000h
    mov es,ax
    cmp byte [es:0efcah],2
    pop es
    jne done
    inc byte [stage]
    mov di,buffer
    mov cx,512
    mov al,0cch
    rep stosb
    call read_sector
    jc done
    cmp ax,1
    jne done
    inc byte [stage]
    mov si,buffer
    mov cx,512
check_data:
    lodsb
    cmp al,31h
    jne done
    loop check_data
    jmp checked
read_sector:
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    ret
table times 11 db 0
checked:
"""


def main():
    """Read through custom and fixed ROM tables on every floppy backend."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    disk = bytearray(360 * 1024)
    disk[:512] = b"\x31" * 512
    formats = {"img": None, "ima": None, "scp": encode_scp, "mfm": encode,
               "86f": encode_86f, "tc": encode_transcopy}
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension, encoder in formats.items():
            image = directory / f"disk.{extension}"
            original = encoder(disk) if encoder else disk
            image.write_bytes(original)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image, CHECKS)
            assert image.read_bytes() == original
            print(f".{extension}: custom DPT and IBM ROM DPT restoration passed")


if __name__ == "__main__":
    main()
