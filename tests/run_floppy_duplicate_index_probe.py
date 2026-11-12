#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Write the same physical duplicate across an index-crossing capture."""
from binascii import crc_hqx
from pathlib import Path
from struct import pack, pack_into
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, DMA, program
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest




def duplicate_cells():
    """Encode two copies with distinct payloads half a turn apart."""
    cells = bytearray()
    previous = 0
    def append(data):
        nonlocal previous
        for byte in data:
            cells.extend(MFM[previous][byte])
            previous = byte & 1
    def sync():
        nonlocal previous
        cells.extend(b'\x44\x89' * 3)
        previous = 1
    def field(data):
        sync()
        append(data + pack('>H', crc_hqx(b'\xa1' * 3 + data, 0xffff)))
    for value, end in ((0x11, 6250), (0x22, 12500)):
        field(bytes((0xfe, 0, 0, 1, 2)))
        append(b'\x4e' * 22 + bytes(12))
        field(b'\xfb' + bytes([value]) * 512)
        append(b'\x4e' * ((end - len(cells)) // 2))
    return cells


def image_bytes(crossing):
    """Move the first duplicate across index on the second revolution."""
    base = duplicate_cells()
    tracks = (base[-64:] + base[:-64],
              base[64:] + base[:64] if crossing else base[-64:] + base[:-64])
    turns = []
    last = 0
    elapsed = 0
    for cells in tracks:
        flux = bytearray()
        bits = ''.join(f'{byte:08b}' for byte in cells)
        for bit, value in enumerate(bits):
            if value == '1':
                edge = elapsed + bit + 1
                flux.extend(pack('>H', (edge - last) * 80))
                last = edge
        elapsed += len(bits)
        turns.append((len(bits) * 80, flux))
    image = bytearray(0x2b0)
    image[:9] = b'SCP' + bytes((0x10, 0x31, 2, 0, 0, 1))
    pack_into('<I', image, 16, len(image))
    image.extend(b'TRK\0')
    offset = 4 + len(turns) * 12
    for duration, flux in turns:
        image.extend(pack('<III', duration, len(flux) // 2, offset))
        offset += len(flux)
    for _, flux in turns:
        image.extend(flux)
    pack_into('<I', image, 12, sum(image[16:]) & 0xffffffff)
    return image


def read_copies(values):
    """Read successive rotating copies and check their payloads."""
    checks = '''
    mov dx,3f2h
    mov al,0ch
    out dx,al
    mov al,1ch
    out dx,al
'''
    for value in values:
        checks += DMA + f'''
    mov si,read_command
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    cmp byte [buffer],{value}
    jne done
    inc byte [stage]
'''
    return checks


COMMANDS = '''
read_command db 46h,0,0,0,1,2,1,54h,0ffh
write_command db 45h,0,0,0,1,2,1,54h,0ffh
'''
def main():
    """Run the regression with an explicitly selected emulator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for crossing in (False, True):
            image = directory / f'crossing{crossing}.scp'
            original = image_bytes(crossing)
            image.write_bytes(original)
            before = (0x11, 0x22, 0x22, 0x11) if crossing else (0x11, 0x22, 0x11, 0x22)
            after = (0x66, 0x22, 0x22, 0x66) if crossing else (0x66, 0x22, 0x66, 0x22)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      program(RESET + read_copies(before), COMMANDS),
                      extra_commands=('config -set cpu cpu_cycles=20000',))
            print(f'crossing={crossing}: before-write copy sequence {before} verified', flush=True)
            checks = RESET + DMA + '''
        mov di,buffer
        mov cx,512
        mov al,66h
        rep stosb
        mov al,4ah
        out 0bh,al
        mov si,write_command
        mov cx,9
        call send_command
        call wait_result
        cmp byte [ids+1],0
        jne done
        inc byte [stage]
    ''' + read_copies(after)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image, program(checks, COMMANDS),
                      extra_commands=('config -set cpu cpu_cycles=20000',), read_only=False)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      program(RESET + read_copies(after), COMMANDS),
                      extra_commands=('config -set cpu cpu_cycles=20000',))
            assert image.read_bytes() == original
            print(f'crossing={crossing}: after-write/restart sequence {after}; '
                  'only the selected physical duplicate changed',
                  flush=True)


if __name__ == "__main__":
    main()
