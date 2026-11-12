#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Retain newly created data fields across other writes and restarts."""
from binascii import crc_hqx
from pathlib import Path
from struct import pack
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, DMA, program
from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest




def cells_with_missing_dam(normal=False):
    """Encode two nonoverlapping IDs, optionally omitting R1's data mark."""
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
    field(bytes((0xfe, 0, 0, 1, 2)))
    # Enough space to write R1's 512-byte data field without touching R2.
    if normal:
        append(b'\x4e' * 22)
        field(b'\xfb' + b'\x11' * 512)
        append(b'\x4e' * 148)
    else:
        append(b'\x4e' * 700)
    field(bytes((0xfe, 0, 0, 2, 2)))
    append(b'\x4e' * 22)
    field(b'\xfb' + b'\x22' * 512)
    append(b'\x4e' * ((12500 - len(cells)) // 2))
    return cells


def transfer(command, write=False, fill=0x66):
    """Issue a single-sector read or write with a known DMA buffer."""
    checks = DMA
    if write:
        checks += f'''
    mov di,buffer
    mov cx,512
    mov al,{fill}
    rep stosb
    mov al,4ah
    out 0bh,al
'''
    return checks + f'''
    mov si,{command}
    mov cx,9
    call send_command
    call wait_result
'''


OK = '''
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    inc byte [stage]
'''
RETAINED = '''
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    cmp byte [buffer],66h
    jne done
'''
COMMANDS = '''
write_first db 45h,0,0,0,1,2,1,54h,0ffh
read_first db 46h,0,0,0,1,2,1,54h,0ffh
write_second db 45h,0,0,0,2,2,2,54h,0ffh
'''

def main():
    """Run the regression with an explicitly selected emulator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ('tc', 'mfm', 'scp', '86f'):
            image = directory / f'idonly.{extension}'
            original = encode_tracks(extension, [cells_with_missing_dam()])
            image.write_bytes(original)
            checks = RESET + transfer('write_first', True) + OK
            checks += transfer('read_first') + OK + '''
        cmp byte [buffer],66h
        jne done
        inc byte [stage]
    '''
            checks += transfer('write_second', True, 0x77) + OK
            checks += transfer('read_first') + RETAINED
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      program(checks, COMMANDS), read_only=False)
            print(f'{extension}: newly written R1 survives a nonoverlapping R2 write', flush=True)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      program(RESET + transfer('read_first') + RETAINED, COMMANDS))
            assert image.read_bytes() == original
            print(f'{extension}: R1 survives restart; source unchanged', flush=True)
        for extension in ('tc', 'mfm', 'scp', '86f'):
            image = directory / f'normal.{extension}'
            original = encode_tracks(extension, [cells_with_missing_dam(True)])
            image.write_bytes(original)
            payload = '''
        cmp byte [buffer],66h
        jne done
    '''
            checks = RESET + transfer('write_first', True) + OK
            checks += transfer('read_first') + OK + payload
            checks += transfer('write_second', True, 0x77) + OK
            checks += transfer('read_first') + OK + payload
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      program(checks, COMMANDS), read_only=False)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      program(RESET + transfer('read_first') + OK + payload, COMMANDS))
            assert image.read_bytes() == original
            print(f'{extension}: normal-DAM control retains R1 after R2 write and restart',
                  flush=True)


if __name__ == "__main__":
    main()
