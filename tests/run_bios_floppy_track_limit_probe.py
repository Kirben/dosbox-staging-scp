#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Honor guest diskette track limits across differently formatted tracks."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack
from tempfile import TemporaryDirectory

from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest


def track(cylinder, size_code, sectors):
    """Encode unique payloads so an incorrect track crossing cannot pass."""
    cells = bytearray()
    previous = 0

    def append(data):
        nonlocal previous
        for byte in data:
            cells.extend(MFM[previous][byte])
            previous = byte & 1

    def field(data):
        nonlocal previous
        append(bytes(12))
        cells.extend(b'\x44\x89' * 3)
        previous = 1
        append(data + pack('>H', crc_hqx(b'\xa1' * 3 + data, 0xffff)))

    append(b'\x4e' * 80)
    for sector in range(int(cylinder == 0), sectors + 1):
        field(bytes((0xfe, cylinder, 0, sector, size_code)))
        append(b'\x4e' * 22)
        field(b'\xfb' + bytes([cylinder * 32 + sector]) * (128 << size_code))
        append(b'\x4e' * 40)
    append(b'\x4e' * ((12500 - len(cells)) // 2))
    return cells


def read_check(sector, count, cylinder, offset, length):
    """Check BIOS status, count and the entire expected payload."""
    return f"""
    mov ax,0200h+{count}
    mov bx,buffer
    mov cx,{cylinder * 256 + sector}
    xor dx,dx
    int 13h
    jc done
    cmp ax,{count}
    jne done
    mov si,buffer
    mov di,expected+{offset}
    mov cx,{length}
    repe cmpsb
    jne done
    inc byte [stage]
"""


def guest(size_code, sectors, written=False):
    """Test full-track reads, explicit shorter limits, Verify, writes and restart."""
    size = 128 << size_code
    first = b''.join(bytes([0xa6 if written else 32 + r]) * size
                     for r in range(1, sectors + 1))
    second = b''.join(bytes([64 + r]) * size for r in range(1, sectors + 1))
    crossing = first[-2 * size:] + second[:2 * size]
    shorter = first[size:3 * size] + second[:2 * size]
    expected = first + second + crossing + shorter + b'\x20' * size
    checks = f"""
    xor ax,ax
    mov es,ax
    les si,[es:78h]
    mov di,table
    mov cx,11
copy_parameters:
    mov al,[es:si]
    mov [di],al
    inc si
    inc di
    loop copy_parameters
    xor ax,ax
    mov es,ax
    mov word [es:78h],table
    mov [es:7ah],ds
    push cs
    pop es
    mov byte [table+3],{size_code}
    mov byte [table+4],{sectors}
"""
    checks += read_check(1, sectors, 1, 0, len(first))
    checks += read_check(sectors - 1, 4, 1, len(first) + len(second), size * 4)
    checks += f"""
    mov ax,0400h+{sectors}
    mov bx,buffer
    mov cx,101h
    xor dx,dx
    int 13h
    jc done
    cmp ax,{sectors}
    jne done
    mov si,buffer
    mov di,expected+{len(first) + len(second)}
    mov cx,{size * 4}
    repe cmpsb
    jne done
    inc byte [stage]
    mov byte [table+4],3
"""
    checks += read_check(2, 4, 1, len(first) + len(second) + size * 4, size * 4)
    checks += """
    mov byte [table+4],0
"""
    checks += read_check(0, 1, 1, len(expected) - size, size)
    checks += read_check(1, 1, 1, 0, size)
    checks += f"    mov byte [table+4],{sectors}\n"
    checks += """
    mov ax,40h
    mov es,ax
    mov si,table
    mov di,0e0h
    mov cx,11
    rep movsb
    xor ax,ax
    mov es,ax
    mov word [es:78h],0e0h
    mov word [es:7ah],40h
    push cs
    pop es
"""
    if not written:
        checks += f"""
    mov di,buffer
    mov cx,{len(first)}
    mov al,0a6h
    rep stosb
    mov ax,0300h+{sectors}
    mov bx,buffer
    mov cx,101h
    xor dx,dx
    int 13h
    jc done
    cmp ax,{sectors}
    jne done
    inc byte [stage]
"""
    checks += read_check(1, sectors, 2, len(first), len(second))
    checks += f"""
    mov di,buffer
    mov cx,{size * 2}
    mov al,0a5h
    rep stosb
    mov ax,0202h
    mov bx,buffer
    mov cx,{512 + sectors}
    xor dx,dx
    int 13h
    jnc done
    cmp ax,0401h
    jne done
    call check_partial
    inc byte [stage]
    mov ax,0402h
    mov bx,buffer
    mov cx,{512 + sectors}
    xor dx,dx
    int 13h
    jnc done
    cmp ax,0401h
    jne done
    call check_partial
    inc byte [stage]
    jmp partial_checked
check_partial:
    mov si,buffer
    mov cx,{size}
partial_payload:
    lodsb
    cmp al,{64 + sectors}
    jne done
    loop partial_payload
    mov cx,{size}
partial_guard:
    lodsb
    cmp al,0a5h
    jne done
    loop partial_guard
    ret
partial_checked:
"""
    checks += "\n    jmp checked\ntable times 11 db 0\nchecked:\n"
    return checks, expected


def main():
    """Exercise four track backends, DMA/PIO and smaller/larger guest limits."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    for machine in ('cga', 'pcjr', 'tandy'):
        for extension in ('tc', 'scp', 'mfm', '86f'):
            for size_code, sectors in ((1, 14), (3, 4)):
                with TemporaryDirectory() as temp:
                    directory = Path(temp)
                    image = directory / f'disk.{extension}'
                    original = encode_tracks(extension, [track(0, 2, 8),
                                             track(1, size_code, sectors),
                                             track(2, size_code, sectors)])
                    image.write_bytes(original)
                    for written in (False, True):
                        checks, expected = guest(size_code, sectors, written)
                        run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                                  checks, expected, machine=machine,
                                  read_only=written)
                    assert image.read_bytes() == original
                    assert image.with_suffix(f'.{extension}.overlay').is_file()
                    print(f'{machine} .{extension} N={size_code}, EOT={sectors}: '
                          'read/Verify/write, track crossings, partial counts, '
                          'relocated/in-place tables, sector-zero EOT and '
                          'restart passed; adjacent track preserved', flush=True)


if __name__ == '__main__':
    main()
