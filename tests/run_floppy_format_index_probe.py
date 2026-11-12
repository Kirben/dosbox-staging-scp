#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Measure Format Track against physical index versus capture-cycle length."""
from pathlib import Path
from struct import pack, pack_into, unpack
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import one_sector, program
from run_formatted_floppy_probe import format_checks
from run_floppy_id_search_probe import wait_microseconds
from run_transcopy_layout_probe import run_guest

CLOCK_SETUP = '''
    cli
    mov al,34h
    out 43h,al
    xor al,al
    out 40h,al
    out 40h,al
    sti
'''

DECLARATIONS = '''
format_command db 4dh,0,2,1,54h,0e5h
filename db 'CLOCK.BIN',0
timestamps times 12 db 0
stamp:
    pushf
    cli
    push eax
    push es
    push word 40h
    pop es
    mov eax,[es:6ch]
    mov [di],eax
    xor al,al
    out 43h,al
    in al,40h
    mov ah,al
    in al,40h
    xchg al,ah
    mov [di+4],ax
    pop es
    pop eax
    popf
    ret
'''

def image_bytes(turn_count):
    """Encode identical turns while keeping flux intervals continuous."""
    cells = one_sector(2)
    bits = ''.join(f'{byte:08b}' for byte in cells)
    turns = []
    last = 0
    for turn in range(turn_count):
        flux = bytearray()
        for position, value in enumerate(bits):
            if value == '1':
                edge = turn * len(bits) + position + 1
                flux.extend(pack('>H', (edge - last) * 80))
                last = edge
        turns.append(flux)
    image = bytearray(0x2b0)
    image[:9] = b'SCP' + bytes((0x10, 0x31, turn_count, 0, 0, 1))
    pack_into('<I', image, 16, len(image))
    image.extend(b'TRK\0')
    offset = 4 + 12 * turn_count
    for flux in turns:
        image.extend(pack('<III', len(bits) * 80, len(flux) // 2, offset))
        offset += len(flux)
    for flux in turns:
        image.extend(flux)
    pack_into('<I', image, 12, sum(image[16:]) & 0xffffffff)
    return image

SAVE = '''
    mov di,timestamps+6
    call stamp
    in al,08h
    test al,4
    jz done
    mov dx,filename
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc done
    mov bx,ax
    mov dx,timestamps
    mov cx,12
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
'''
# pylint: disable-next=too-many-locals
def main():
    """Run the regression with an explicitly selected emulator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for turns, delay in ((1, 0), (2, 0), (5, 0), (1, 250000),
                             (2, 250000), (5, 250000)):
            image = directory / f'turns{turns}-delay{delay}.scp'
            original = image_bytes(turns)
            image.write_bytes(original)
            checks = format_checks(2).split('    call read_id', maxsplit=1)[0]
            checks = checks.replace('    mov si,format_command',
                                    (wait_microseconds(delay) if delay else '') +
                                    '    mov di,timestamps\n    call stamp\n'
                                    '    mov si,format_command')
            checks = CLOCK_SETUP + checks + SAVE
            body = program(checks, DECLARATIONS).replace('mov cx,50', 'mov cx,200')
            run_guest(args.dosbox.resolve(),
                      args.nasm, directory, image, body, read_only=False,
                      extra_commands=('config -set cpu cpu_cycles=20000',))
            t0, p0, t1, p1 = unpack('<IHIH', (directory / 'CLOCK.BIN').read_bytes())
            elapsed = (65536 * (t1 - t0) + p0 - p1) * 1000 / 1193182
            expected = 400 if not delay else 350
            assert abs(elapsed - expected) < 25, (turns, delay, elapsed)
            assert image.read_bytes() == original
            print(f'{turns} revolutions: Format Track {elapsed:.3f} ms; '
                  'status success, DMA TC, source unchanged', flush=True)


if __name__ == "__main__":
    main()
