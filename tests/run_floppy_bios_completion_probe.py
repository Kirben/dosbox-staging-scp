#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check BIOS consumes its own completion flags without hiding direct IRQs."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_fat_floppy_cache_probe import fat12
from run_hxc_mfm_probe import encode
from run_scp_attachment_probe import encode_scp
from run_transcopy_attachment_probe import encode_transcopy
from run_transcopy_layout_probe import run_guest
from run_floppy_id_search_probe import wait_microseconds


def bios_checks(track):
    """Exercise successful reads, Verify and failing writes/missing reads."""
    checks = ""
    for stage, function, sector, error in ((1, 2, 1, 0), (2, 4, 1, 0),
                                           (3, 3, 1, 3), (4, 2, 63, 4)):
        if stage == 4 and not track:
            continue
        flags = 0x25 if track else 0xa5
        checks += f"""
    mov byte [stage],{stage}
    xor ax,ax
    mov es,ax
    mov byte [es:43eh],0a5h
    push cs
    pop es
    mov ax,{(function << 8) | 1}
    mov bx,buffer
    mov cx,{sector}
    xor dx,dx
    int 13h
    {'jnc' if error else 'jc'} done
    cmp ah,{error}
    jne done
    xor ax,ax
    mov es,ax
    cmp byte [es:43eh],{flags}
    jne done
"""
    return checks


DIRECT = """
    ; Do not clear BDA bit 7: the BIOS must already have consumed it.
    mov dx,3f2h
    mov al,1ch
    out dx,al
    mov dx,3f5h
    mov al,7
    out dx,al
    xor al,al
    out dx,al
""" + wait_microseconds(20000) + """
    xor ax,ax
    mov es,ax
    test byte [es:43eh],80h
    jz done
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,20h
    jne done
    in al,dx
    cmp al,0
    jne done
    ; Direct commands leave their completion flag for the guest to consume.
    test byte [es:43eh],80h
    jz done
"""


def main():
    """Run the completion handoff on six read-only image formats."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        disk = fat12()
        for extension, encoder in (("img", bytes), ("ima", bytes), ("scp", encode_scp),
                                   ("mfm", encode), ("86f", encode_86f),
                                   ("tc", encode_transcopy)):
            image = directory / f"completion.{extension}"
            original = encoder(disk)
            image.write_bytes(original)
            for machine in ("cga", "pcjr", "tandy"):
                checks = bios_checks(extension not in ("img", "ima")) + """
    mov byte [stage],5
    xor ax,ax
    xor dx,dx
    int 13h
    jc done
    xor ax,ax
    mov es,ax
    test byte [es:43eh],80h
    jnz done
"""
                if machine != "pcjr":
                    checks += DIRECT
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          checks, machine=machine)
                assert image.read_bytes() == original
                print(f"{extension} {machine}: BIOS read/Verify/errors/reset completion "
                      "and direct IRQ handoff passed", flush=True)


if __name__ == "__main__":
    main()
