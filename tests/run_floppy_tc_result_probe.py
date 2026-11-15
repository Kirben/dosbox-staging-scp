#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check the next CHRN reported after normal DMA terminal count."""
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, DMA, one_sector, program
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
        for extension in ('img', 'ima', 'tc', 'mfm', 'scp', '86f'):
            for writing in (False, True):
                for eot in (2, 1, 255):
                    image = directory / f'tc{writing}{eot}.{extension}'
                    original = bytearray(360 * 1024) if extension in ('img', 'ima') else None
                    if original is not None:
                        original[:512] = b'\x77' * 512
                    else:
                        original = encode_tracks(extension, [one_sector(2)])
                    image.write_bytes(original)
                    checks = RESET + DMA
                    if writing:
                        checks += '''
        mov di,buffer
        mov cx,512
        mov al,77h
        rep stosb
        mov al,4ah
        out 0bh,al
    '''
                    checks += f'''
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
        in al,08h
        test al,4
        jz done
        cmp byte [buffer],77h
        jne done
        cmp word [ids+3],{0 if eot != 1 else 1}
        jne done
        cmp word [ids+5],{0x0202 if eot != 1 else 0x0201}
        jne done
    '''
                    command = f'command db {0x45 if writing else 0x46},0,0,0,1,2,{eot},54h,0ffh\n'
                    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                              program(checks, command),
                              read_only=not writing)
                    assert image.read_bytes() == original
                    expected = 'C0/H0/R2/N2' if eot != 1 else 'C1/H0/R1/N2'
                    print(f'{extension} {"Write" if writing else "Read"} Data EOT={eot}: '
                          f'data, DMA TC and next position {expected} passed',
                          flush=True)
            if extension in ('img', 'ima'):
                check_missing(args, directory, extension)


def check_missing(args, directory, extension):
    """Stop on a genuinely missing ID without accessing the next raw head."""
    image = directory / f'missing.{extension}'
    for writing in (False, True):
        original = bytearray(b'\x99' * (360 * 1024))
        original[8 * 512:9 * 512] = b'\x33' * 512
        image.write_bytes(original)
        checks = RESET + DMA.replace('mov cx,512', 'mov cx,1040').replace(
            'mov al,1\n    out 05h,al', 'mov al,3\n    out 05h,al')
        if writing:
            checks += 'mov di,buffer\nmov cx,1024\nmov al,55h\nrep stosb\n'
            checks += 'mov al,4ah\nout 0bh,al\n'
        checks += '''
    push es
    mov ax,40h
    mov es,ax
    mov ax,[es:6ch]
    mov [start_tick],ax
    pop es
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    cmp word [ids],0440h
    jne done
    cmp byte [ids+2],0
    jne done
    cmp byte [ids+5],10
    jne done
    in al,08h
    test al,4
    jnz done
    xor al,al
    out 0ch,al
    in al,05h
    cmp al,0ffh
    jne done
    in al,05h
    cmp al,1
    jne done
    push es
    mov ax,40h
    mov es,ax
    mov ax,[es:6ch]
    pop es
    sub ax,[start_tick]
    cmp ax,5
    jb done
'''
        checks += f'''
    mov di,buffer
    mov cx,512
    mov al,{0x55 if writing else 0x33}
    repe scasb
    jne done
    mov cx,512
    mov al,{0x55 if writing else 0xa5}
    repe scasb
    jne done
    mov cx,16
    mov al,0a5h
    repe scasb
    jne done
'''
        commands = (f'command db {0x45 if writing else 0x46},0,0,0,9,2,255,54h,0ffh\n'
                    'start_tick dw 0\n')
        run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                  program(checks, commands), read_only=not writing)
        if writing:
            original[8 * 512:9 * 512] = b'\x55' * 512
            replay = RESET + DMA + '''
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    cmp word [ids],0
    jne done
    mov di,buffer
    mov cx,512
    mov al,55h
    repe scasb
    jne done
'''
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      program(replay, 'command db 46h,0,0,0,9,2,255,54h,0ffh\n'))
        assert image.read_bytes() == original
    print(f'{extension}: missing ID timeout, partial DMA, IRQ6, guards and restart passed',
          flush=True)


if __name__ == "__main__":
    main()
