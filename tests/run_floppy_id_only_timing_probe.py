#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare DMA write duration for ID-only and ordinary data fields."""
from pathlib import Path
from struct import unpack
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, DMA, program
from run_floppy_zero_sector_probe import encode_tracks
from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS, SAVE
from run_floppy_read_track_size_probe import ROUTINES
from run_floppy_id_only_write_probe import cells_with_missing_dam
from run_transcopy_layout_probe import run_guest


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
            for missing in (False, True):
                image = directory / f'idonly{missing}.{extension}'
                original = encode_tracks(extension, [cells_with_missing_dam(not missing)])
                image.write_bytes(original)
                checks = RESET + DMA + CLOCK_SETUP + '''
        mov di,buffer
        mov cx,512
        mov al,66h
        rep stosb
        mov al,4ah
        out 0bh,al
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
        cmp byte [ids],0
        jne done
        cmp byte [ids+1],0
        jne done
        cmp byte [ids+2],0
        jne done
    ''' + SAVE
                commands = DECLARATIONS + '\ncommand db 45h,0,0,0,1,2,1,54h,0ffh\n'
                body = checks + '\n    jmp checked\n' + commands + ROUTINES + 'checked:\n'
                run_guest(args.dosbox.resolve(), args.nasm, directory, image, body, read_only=False,
                          extra_commands=('config -set cpu cpu_cycles=20000',))
                t0,p0,t1,p1 = unpack('<IHIH', (directory / 'CLOCK.BIN').read_bytes())
                elapsed = (65536*(t1-t0)+p0-p1)*1000/1193182
                assert 20.928 <= elapsed < 21.1, (missing, elapsed)
                saved = Path(str(image) + '.overlay').read_bytes()
                readback = RESET + DMA + '''
        mov si,read_command
        mov cx,9
        call send_command
        call wait_result
        cmp byte [ids+1],0
        jne done
        cmp byte [ids+2],0
        jne done
        mov si,buffer
        mov cx,512
    payload:
        lodsb
        cmp al,66h
        jne done
        loop payload
    '''
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          program(readback, 'read_command db 46h,0,0,0,1,2,1,54h,0ffh\n'))
                assert Path(str(image) + '.overlay').read_bytes() == saved
                assert image.read_bytes() == original
                print(f'.{extension} {"ID-only" if missing else "normal DAM"}: '
                      f'write success at {elapsed:.3f} ms; DMA TC, full restart payload '
                      'and CRC verified; source unchanged', flush=True)


if __name__ == "__main__":
    main()
