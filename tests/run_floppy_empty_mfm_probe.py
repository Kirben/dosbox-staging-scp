#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare zero-length and encoded blank HxC tracks at 300/360 RPM."""
from pathlib import Path
from struct import pack_into, unpack
from tempfile import TemporaryDirectory
import argparse

from run_floppy_id_search_probe import head_track, wait_microseconds
from run_floppy_transfer_edges_probe import RESET, DMA, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest
from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS, SAVE
from run_formatted_floppy_probe import format_checks


def check_format(args, directory, image, original, rpm):
    """Format the blank cylinder using its RPM, then read it after restart."""
    seek = '''
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
'''
    checks = CLOCK_SETUP + '''
    mov di,timestamps
    call stamp
''' + RESET + seek
    checks += format_checks(2).split('    call read_id', maxsplit=1)[0].replace(
        '    mov si,format_command', '    mov byte [buffer],1\n    mov si,format_command')
    commands = DECLARATIONS + 'seek db 0fh,0,1\n'
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              program(checks + SAVE, commands), read_only=False)
    t0, p0, t1, p1 = unpack('<IHIH', (directory / 'CLOCK.BIN').read_bytes())
    elapsed = (65536 * (t1 - t0) + p0 - p1) * 1000 / 1193182
    assert abs(elapsed - 120000 / rpm) < 25, (rpm, elapsed)
    saved = Path(str(image) + '.overlay').read_bytes()
    read = RESET + seek + DMA + '''
    mov si,read_command
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    cmp byte [buffer],0e5h
    jne done
'''
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              program(read, 'seek db 0fh,0,1\n'
                      'read_command db 46h,0,1,0,1,2,1,54h,0ffh\n'))
    assert Path(str(image) + '.overlay').read_bytes() == saved
    assert image.read_bytes() == original
    print(f'{image.name}: format {elapsed:.3f} ms; restart read passed', flush=True)

def main():
    """Run the regression with an explicitly selected emulator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for rpm in (300, 360):
            for empty in (False, True):
                image = directory / f'rpm{rpm}-empty{empty}.mfm'
                original = bytearray(encode_tracks('mfm', [head_track(0), b'\x92\x54' * 6250]))
                pack_into('<H', original, 10, rpm)
                if empty:
                    pack_into('<I', original, 19 + 11 + 3, 0)
                image.write_bytes(original)
                finished = rpm == 360
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
    ''' + wait_microseconds(350000) + f'''
        mov dx,3f4h
        in al,dx
        cmp al,{0xd0 if finished else 0x10}
        jne done
        inc byte [stage]
    ''' + wait_microseconds(100000) + '''
        call wait_result
        cmp byte [ids+1],1
        jne done
        cmp byte [ids+2],0
        jne done
        in al,08h
        test al,4
        jnz done
        cmp byte [buffer],0a5h
        jne done
    '''
                commands = 'seek db 0fh,0,1\ncommand db 46h,0,1,0,1,2,1,54h,0ffh\n'
                run_guest(args.dosbox.resolve(),
                          args.nasm, directory, image, program(checks, commands))
                assert image.read_bytes() == original
                print(f'{rpm} RPM, {"zero-length" if empty else "encoded blank"} track: '
                      f'{"finished" if finished else "still searching"} at 350 ms; '
                      'MA, IRQ6, no DMA TC and unchanged source verified', flush=True)
                check_format(args, directory, image, original, rpm)


if __name__ == "__main__":
    main()
