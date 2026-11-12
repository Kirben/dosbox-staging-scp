#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check drive-busy reporting during Seek and before Sense Interrupt."""
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_floppy_id_search_probe import wait_microseconds
from run_transcopy_layout_probe import run_guest


def recovery_checks(unit):
    """Check failed seeks, reset cancellation and a subsequent recalibrate."""
    return f'''
    mov dx,3f2h
    mov al,{0x0c | unit}
    out dx,al
    mov si,seek
    mov cx,3
    call send_command
''' + wait_microseconds(10000) + f'''
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,{0x68 | unit}
    jne done
    in al,dx
    cmp al,0
    jne done
    inc byte [stage]
    mov dx,3f2h
    mov al,{0x1c if unit == 0 else 0x2d}
    out dx,al
    mov si,seek
    mov cx,3
    call send_command
    mov dx,3f2h
    xor al,al
    out dx,al
    mov al,{0x1c if unit == 0 else 0x2d}
    out dx,al
    call drain_reset
''' + wait_microseconds(10000) + '''
    mov dx,3f4h
    in al,dx
    cmp al,80h
    jne done
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,80h
    jne done
    inc byte [stage]
    mov si,recalibrate
    mov cx,2
    call send_command
''' + wait_microseconds(10000) + f'''
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,{0x20 | unit}
    jne done
    in al,dx
    cmp al,0
    jne done
'''


def main():
    """Run the guest regression against the selected emulator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ('img', 'ima', 'tc', 'mfm', 'scp', '86f'):
            image = directory / f'busy.{extension}'
            original = bytes(360 * 1024) if extension in ('img', 'ima') else encode_tracks(
                extension, [one_sector(2)] * 3)
            image.write_bytes(original)
            for unit in (0, 1):
                checks = RESET + f'''
        mov dx,3f2h
        mov al,{0x1c if unit == 0 else 0x2d}
        out dx,al
    '''
                commands = f'seek db 0fh,{unit},2\nrecalibrate db 7,{unit}\n'
                commands += 'specify db 3,0dfh,0\n'
                for name, length, cylinder in (('seek', 3, 2), ('recalibrate', 2, 0)):
                    checks += f'''
        mov si,{name}
        mov cx,{length}
        call send_command
        mov dx,3f4h
        in al,dx
        cmp al,{0x80 | (1 << unit)}
        jne done
        inc byte [stage]
        mov dx,3f5h
        mov al,8
        out dx,al
        in al,dx
        cmp al,80h
        jne done
        mov si,specify
        mov cx,3
        call send_command
    ''' + wait_microseconds(10000) + f'''
        mov dx,3f4h
        in al,dx
        cmp al,{0x80 | (1 << unit)}
        jne done
        inc byte [stage]
        push ds
        xor ax,ax
        mov ds,ax
        test byte [043eh],80h
        pop ds
        jz done
        mov dx,3f5h
        mov al,8
        out dx,al
        in al,dx
        cmp al,{0x20 | unit}
        jne done
        in al,dx
        cmp al,{cylinder}
        jne done
        inc byte [stage]
        mov dx,3f4h
        in al,dx
        cmp al,80h
        jne done
    '''
                checks += recovery_checks(unit)
                extra = () if not unit else (f'mount 1 "{image}" -t floppy -fs none -ro',)
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          program(checks, commands),
                          extra_commands=extra)
                assert image.read_bytes() == original
                print(f'.{extension} drive {"AB"[unit]} Seek and Recalibrate: '
                      'drive busy retained through completion until Sense Interrupt; '
                      f'bit {1 << unit:02x} clears after acknowledgement; '
                      'IRQ6, seek-end and final cylinders verified', flush=True)


if __name__ == "__main__":
    main()
