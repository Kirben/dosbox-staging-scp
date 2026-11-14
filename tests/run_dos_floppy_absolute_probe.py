#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check DOS absolute floppy I/O, BIOS hooks and logical BPB geometry."""

import argparse
from pathlib import Path
from struct import pack_into
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_fat_floppy_cache_probe import fat12
from run_hxc_mfm_probe import encode
from run_scp_attachment_probe import encode_scp
from run_transcopy_attachment_probe import encode_transcopy
from run_transcopy_layout_probe import run_guest


def absolute(operation, sector, unit, packet=False):
    """Keep the extra flags word left by INT 25h/26h out of the guest stack."""
    return f"""
    mov ax,{unit}
    mov bx,{'packet' if packet else 'buffer'}
    mov cx,{0xffff if packet else 1}
    mov dx,{sector}
    int {operation}h
    pushf
    pop word [returned_flags]
    pop word [extra_flags]
"""


def check_data(marker):
    """Check every byte, including a guard after the sector."""
    return f"""
    push cs
    pop es
    mov si,buffer
    mov cx,512
check_{marker}:
    lodsb
    cmp al,{marker}
    jne done
    loop check_{marker}
    cmp word [buffer+512],0a5a5h
    jne done
    inc byte [stage]
"""


def guest(unit, restart):
    """A Sierra-style hook must see INT 13h without a hidden buffer write."""
    checks = """
    mov di,buffer
    mov cx,514
    mov al,0a5h
    rep stosb
    xor ax,ax
    mov es,ax
    mov ax,[es:4ch]
    mov [old_bios],ax
    mov ax,[es:4eh]
    mov [old_bios+2],ax
    mov word [es:4ch],hook
    mov [es:4eh],cs
    push cs
    pop es
    mov si,1111h
    mov di,2222h
    mov bp,3333h
"""
    checks += absolute(25, 8, unit)
    checks += f"""
    ; Only carry is defined as the DOS result.
    test word [returned_flags],1
    jnz done
    test ax,ax
    jnz done
    cmp byte [hook_count],1
    jne done
    cmp byte [hook_unit],{unit}
    jne done
    cmp word [hook_cx],0101h
    jne done
    cmp si,1111h
    jne done
    cmp di,2222h
    jne done
    cmp bp,3333h
    jne done
    inc byte [stage]
"""
    checks += check_data(0xa5)
    checks += """
    xor ax,ax
    mov es,ax
    mov ax,[old_bios]
    mov [es:4ch],ax
    mov ax,[old_bios+2]
    mov [es:4eh],ax
    push cs
    pop es
    mov word [packet+6],buffer
    mov [packet+8],ds
"""
    checks += absolute(25, 8, unit, packet=True)
    checks += "    test word [returned_flags],1\n    jnz done\n"
    checks += check_data(0x81)
    checks += absolute(25, 6, unit)
    checks += "    test word [returned_flags],1\n    jnz done\n"
    checks += check_data(0x72 if restart else 0x31)
    checks += """
    mov di,buffer
    mov cx,512
    mov al,72h
    rep stosb
"""
    checks += absolute(26, 6, unit)
    checks += f"""
    test word [returned_flags],1
    {'jz' if restart else 'jnz'} done
    cmp ax,{0x0408 if restart else 0}
    jne done
    inc byte [stage]
    jmp checked
hook:
    inc byte [cs:hook_count]
    mov [cs:hook_unit],dl
    mov [cs:hook_cx],cx
    ; Like Sierra's probe, return success without transferring anything.
    ; Clobber additional registers to check the DOS adapter's restoration.
    mov bx,0aaaah
    mov cx,0bbbbh
    mov dx,0cccch
    mov si,0ddddh
    mov di,0eeeeh
    mov bp,0ffffh
    mov ax,1234h
    mov es,ax
    mov ds,ax
    std
    xor ah,ah
    clc
    retf 2
old_bios dd 0
hook_count db 0
hook_unit db 0ffh
hook_cx dw 0
returned_flags dw 0
extra_flags dw 0
packet dd 8
    dw 1,0,0
checked:
"""
    return checks


def main():
    """Exercise six formats, both slots, restart and read-only controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    disk = fat12()
    # The BPB's one-head, eight-sector logical layout differs from the
    # physical two-head, nine-sector captures. Logical LBA 8 is C1/H0/R1.
    pack_into('<H', disk, 19, 320)
    pack_into('<HH', disk, 24, 8, 1)
    disk[8 * 512:9 * 512] = b'\x82' * 512
    disk[18 * 512:19 * 512] = b'\x81' * 512
    encoders = (('img', bytes), ('ima', bytes), ('scp', encode_scp),
                ('tc', encode_transcopy), ('mfm', encode), ('86f', encode_86f))
    for index, (extension, encoder) in enumerate(encoders):
        with TemporaryDirectory() as temp:
            directory = Path(temp)
            image = directory / f'disk.{extension}'
            original = encoder(disk)
            image.write_bytes(original)
            unit = index % 2
            for restart in (False, True):
                access = ' -ro' if restart else ''
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          guest(unit, restart), read_only=True,
                          extra_commands=(f'mount {"ab"[unit]} "{image}"{access}',))
            if extension in ('scp', 'tc', 'mfm', '86f'):
                assert image.read_bytes() == original
                assert image.with_suffix(f'.{extension}.overlay').is_file()
            print(f'.{extension} {"AB"[unit]}: INT 25h hook, BPB geometry, reads, '
                  'INT 26h persistence and read-only rejection passed', flush=True)


if __name__ == '__main__':
    main()
