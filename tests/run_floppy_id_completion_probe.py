#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Measure Read ID result phase against the recorded ID field and CRC."""
from pathlib import Path
from struct import unpack
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, DMA, one_sector
from run_floppy_zero_sector_probe import encode_tracks
from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS
from run_floppy_read_track_size_probe import ROUTINES
from run_transcopy_layout_probe import run_guest


SAVE = '''
    mov di,timestamps+6
    call stamp
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
    """Run the guest regression against the selected emulator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ('tc', 'mfm', 'scp', '86f'):
            for bad_crc in (False, True):
                cells = bytearray(one_sector(2))
                if bad_crc:
                    cells[201] ^= 1  # One data bit in the first ID CRC byte.
                image = directory / f'idcrc{bad_crc}.{extension}'
                original = encode_tracks(extension, [cells])
                image.write_bytes(original)
                checks = RESET + DMA + CLOCK_SETUP + f'''
        mov dx,3f2h
        mov al,0ch
        out dx,al
        mov di,timestamps
        call stamp
        mov dx,3f2h
        mov al,1ch
        out dx,al
        mov si,command
        mov cx,2
        call send_command
        call wait_result
        cmp byte [ids],{0x40 if bad_crc else 0}
        jne done
        cmp byte [ids+1],{0x20 if bad_crc else 0}
        jne done
        cmp byte [ids+2],0
        jne done
        cmp word [ids+3],0
        jne done
        cmp word [ids+5],0201h
        jne done
        in al,08h
        test al,4
        jnz done
    ''' + SAVE
                body = checks + '\n    jmp checked\n' + DECLARATIONS + '\ncommand db 4ah,0\n'
                body += ROUTINES + 'checked:\n'
                run_guest(args.dosbox.resolve(), args.nasm, directory, image, body,
                          extra_commands=('config -set cpu cpu_cycles=20000',))
                t0,p0,t1,p1 = unpack('<IHIH', (directory / 'CLOCK.BIN').read_bytes())
                elapsed = (65536*(t1-t0)+p0-p1)*1000/1193182
                assert 3.264 <= elapsed < 3.35, (extension, bad_crc, elapsed)
                assert image.read_bytes() == original
                print(f'.{extension} ID CRC {"bad" if bad_crc else "good"}: '
                      f'result at {elapsed:.3f} ms; ID sync starts 2.944 ms, '
                      'CRC ends 3.264 ms; CHRN/status, no DMA TC and source verified', flush=True)


if __name__ == "__main__":
    main()
