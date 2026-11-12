#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Report missing ID marks for blank-track Read Track and Read Data."""
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse

from run_floppy_id_search_probe import head_track, wait_microseconds
from run_floppy_transfer_edges_probe import RESET, DMA, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest

def main():
    """Run the regression with an explicitly selected emulator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ('scp', 'mfm', '86f', 'tc'):
            image = directory / f'blank.{extension}'
            original = encode_tracks(extension, [head_track(0), b'\x92\x54' * 6250])
            image.write_bytes(original)
            for opcode in (0x46, 0x42):
                checks = RESET + DMA + '''
        mov si,seek
        mov cx,3
        call send_command
    ''' + wait_microseconds(10000) + '''
        mov dx,3f5h
        mov al,8
        out dx,al
        in al,dx
        in al,dx
        cmp al,1
        jne done
        inc byte [stage]
        mov si,command
        mov cx,9
        call send_command
    ''' + wait_microseconds(250000) + '''
        mov dx,3f4h
        in al,dx
        cmp al,10h
        jne done
        inc byte [stage]
    ''' + wait_microseconds(200000) + '''
        call wait_result
        cmp byte [ids],40h
        jne done
        cmp byte [ids+1],1
        jne done
        cmp byte [ids+2],0
        jne done
        in al,08h
        test al,4
        jnz done
        mov si,buffer
        mov cx,512
    check_guard:
        lodsb
        cmp al,0a5h
        jne done
        loop check_guard
    '''
                declarations = f'seek db 0fh,0,1\ncommand db {opcode},0,1,0,1,2,1,54h,0ffh\n'
                run_guest(args.dosbox.resolve(),
                          args.nasm, directory, image, program(checks, declarations))
                assert image.read_bytes() == original
                print(f'.{extension} {"Read Track" if opcode == 0x42 else "Read Data"}: '
                      'ST1=01, '
                      f'still searching at 250 ms; '
                      'IRQ6, untouched DMA buffer and no TC verified', flush=True)



if __name__ == "__main__":
    main()
