#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check IBM BIOS deleted-sector skipping and its saved FDC results."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack
from tempfile import TemporaryDirectory

from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest


def track(cylinder):
    """Include deleted marks, special IDs and distinct ID/data CRC errors."""
    cells = bytearray()
    previous = 0

    def append(data):
        nonlocal previous
        for byte in data:
            cells.extend(MFM[previous][byte])
            previous = byte & 1

    def field(data, bad_crc=False):
        nonlocal previous
        append(bytes(12))
        cells.extend(b'\x44\x89' * 3)
        previous = 1
        crc = crc_hqx(b'\xa1' * 3 + data, 0xffff) ^ int(bad_crc)
        append(data + pack('>H', crc))

    append(b'\x4e' * 80)
    ids = range(1, 10) if cylinder == 0 else (1, 2, 3, 8, 9, 20, 21, 22, 49, 50)
    for sector in ids:
        field(bytes((0xfe, cylinder, 0, sector, 2)), sector == 50)
        append(b'\x4e' * 22)
        mark = 0xf8 if cylinder and sector in (2, 8, 21) else 0xfb
        field(bytes([mark]) + bytes([sector]) * 512, sector == 49)
        append(b'\x4e' * 40)
    append(b'\x4e' * ((12500 - len(cells)) // 2))
    return cells


def read_check(sector, payload, status, result):
    """Check the BIOS, DMA, IRQ6 and BDA observations of one command."""
    return f"""
    mov di,buffer
    mov cx,514
    mov al,0a5h
    rep stosb
    mov ax,0201h
    mov bx,buffer
    mov cx,100h+{sector}
    xor dx,dx
    mov byte [interrupts],0
    int 13h
    pushf
    cmp ax,{status * 256 + int(status == 0)}
    jne failed_flags
    pop ax
    and ax,1
    cmp ax,{int(status != 0)}
    jne done
    cmp byte [interrupts],1
    jne done
    mov ax,40h
    mov es,ax
    cmp byte [es:41h],{status}
    jne done
    cmp word [es:42h],{result[0] + result[1] * 256}
    jne done
    cmp byte [es:44h],{result[2]}
    jne done
    push ds
    pop es
    mov si,buffer
    mov cx,512
check_data_{sector}:
    lodsb
    cmp al,{payload}
    jne done
    loop check_data_{sector}
    cmp word [buffer+512],0a5a5h
    jne done
    inc byte [stage]
"""


def checks():
    """Install an IRQ counter and use an owned copy of the guest DPT."""
    source = """
    xor ax,ax
    mov es,ax
    mov ax,[es:38h]
    mov [old_irq],ax
    mov ax,[es:3ah]
    mov [old_irq+2],ax
    mov word [es:38h],irq
    mov [es:3ah],cs
    les si,[es:78h]
    mov di,table
    mov cx,11
copy_table:
    mov al,[es:si]
    mov [di],al
    inc si
    inc di
    loop copy_table
    xor ax,ax
    mov es,ax
    mov word [es:78h],table
    mov [es:7ah],ds
    mov byte [table+3],2
    mov byte [table+4],9
    push ds
    pop es
"""
    for sector, payload, status, result in (
            (1, 1, 0, (0, 0, 0)),
            (2, 3, 0, (0, 0, 0x40)),
            (8, 9, 0, (0, 0, 0x40)),
            (20, 20, 0, (0, 0, 0)),
            (21, 22, 0, (0, 0, 0x40)),
            (49, 49, 0x10, (0x40, 0x20, 0x20)),
            (50, 0xa5, 0x10, (0x40, 0x20, 0))):
        source += read_check(sector, payload, status, result)
    source += """
    jmp checked
failed_flags:
    pop ax
    jmp done
irq:
    inc byte [cs:interrupts]
    jmp far [cs:old_irq]
old_irq dd 0
interrupts db 0
table times 11 db 0
checked:
"""
    return source


def main():
    """Run the same guest-visible contract on each track-image backend."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    for machine in ('cga', 'tandy'):
        for extension in ('tc', 'scp', 'mfm', '86f'):
            with TemporaryDirectory() as temp:
                directory = Path(temp)
                image = directory / f'disk.{extension}'
                original = encode_tracks(extension, [track(0), track(1)])
                image.write_bytes(original)
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          checks(), machine=machine)
                assert image.read_bytes() == original
                print(f'{machine} .{extension}: BIOS SK, guest EOT, special IDs, '
                      'CRC results, IRQ6 and buffer guards passed', flush=True)


if __name__ == '__main__':
    main()
