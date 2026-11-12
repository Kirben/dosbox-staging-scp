#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check ID CRC error completion timing in Read, Write and Verify."""
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
            for name, opcode in (('Read Data',0x46), ('Write Data',0x45), ('Verify',0x56)):
                cells = bytearray(one_sector(2))
                cells[201] ^= 1
                image = directory / f'idcrc{opcode}.{extension}'
                original = encode_tracks(extension, [cells])
                image.write_bytes(original)
                checks = RESET + DMA + CLOCK_SETUP
                if opcode == 0x45:
                    checks += '    mov al,4ah\n    out 0bh,al\n'
                checks += '''
        mov dx,3f2h
        mov al,0ch
        out dx,al
        mov di,timestamps
        call stamp
        mov dx,3f2h
        mov al,1ch
        out dx,al
        mov si,command
        mov cx,9
        call send_command
        call wait_result
        mov di,timestamps+6
        call stamp
        cmp byte [ids],40h
        jne done
        cmp byte [ids+1],20h
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
        mov si,buffer
        mov cx,512
    guard:
        lodsb
        cmp al,0a5h
        jne done
        loop guard
    ''' + SAVE
                commands = DECLARATIONS + f'\ncommand db {opcode},0,0,0,1,2,1,54h,0ffh\n'
                body = checks + '\n    jmp checked\n' + commands + ROUTINES + 'checked:\n'
                run_guest(args.dosbox.resolve(), args.nasm, directory, image, body,
                          read_only=opcode != 0x45,
                          extra_commands=('config -set cpu cpu_cycles=20000',))
                t0,p0,t1,p1 = unpack('<IHIH', (directory / 'CLOCK.BIN').read_bytes())
                elapsed = (65536*(t1-t0)+p0-p1)*1000/1193182
                assert 3.264 <= elapsed < 3.35, (extension, name, elapsed)
                assert image.read_bytes() == original
                assert not Path(str(image) + '.overlay').exists()
                print(f'.{extension} {name}: bad-ID-CRC result at {elapsed:.3f} ms; '
                      'CRC ends 3.264 ms; status, untouched buffer, no TC/overlay verified',
                      flush=True)


if __name__ == "__main__":
    main()
