#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare PIO underrun completion with a full DMA sector write."""
from pathlib import Path
from struct import unpack
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, DMA, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS
from run_floppy_read_track_size_probe import ROUTINES
from run_transcopy_layout_probe import run_guest
from run_floppy_id_search_probe import wait_microseconds


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
def check_abort(args, directory, extension, reset):
    """Cancel a pending underrun before the physical commit and recover."""
    image = directory / f'abort{reset}.{extension}'
    original = encode_tracks(extension, [one_sector(2)])
    image.write_bytes(original)
    checks = RESET + '''
    mov si,specify_pio
    mov cx,3
    call send_command
    mov si,write_command
    mov cx,9
    call send_command
''' + wait_microseconds(210000) + f'''
    mov dx,3f4h
    in al,dx
    cmp al,30h
    jne done
    inc byte [stage]
    mov dx,3f2h
    mov al,{0 if reset else 0x0c}
    out dx,al
    mov al,1ch
    out dx,al
'''
    if reset:
        checks += '    call drain_reset\n'
    else:
        checks += '''
    call wait_result
    cmp byte [ids+1],4
    jne done
'''
    checks += wait_microseconds(25000) + '''
    mov si,specify_dma
    mov cx,3
    call send_command
''' + DMA + '''
    mov si,read_command
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    mov di,buffer
    mov cx,512
    mov al,77h
    repe scasb
    jne done
'''
    commands = ('specify_pio db 3,0dfh,3\nspecify_dma db 3,0dfh,2\n'
                'write_command db 45h,0,0,0,1,2,1,54h,0ffh\n'
                'read_command db 46h,0,0,0,1,2,1,54h,0ffh\n')
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              program(checks, commands), read_only=False)
    assert not Path(str(image) + '.overlay').exists()
    assert image.read_bytes() == original


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
            for supplied in (None, 0, 1, 16):
                pio = supplied is not None
                image = directory / f'underrun{supplied}.{extension}'
                original = encode_tracks(extension, [one_sector(2)])
                image.write_bytes(original)
                setup = RESET + CLOCK_SETUP
                if pio:
                    setup += '''
        mov si,specify
        mov cx,3
        call send_command
    '''
                else:
                    setup += DMA + '''
        mov di,buffer
        mov cx,512
        mov al,66h
        rep stosb
        mov al,4ah
        out 0bh,al
    '''
                setup += '''
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
    '''
                if pio and supplied:
                    setup += f'''
        mov bp,{supplied}
        mov ecx,2000000
    poll_byte:
        mov dx,3f4h
        in al,dx
        and al,0f0h
        cmp al,0b0h
        je ready
        dec ecx
        jnz poll_byte
        jmp done
    ready:
        mov dx,3f5h
        mov al,66h
        out dx,al
        dec bp
        jnz poll_byte
    '''
                setup += f'''
        call wait_result
        cmp byte [ids],{0x40 if pio else 0}
        jne done
        cmp byte [ids+1],{0x10 if pio else 0}
        jne done
        cmp byte [ids+2],0
        jne done
    ''' + SAVE
                commands = DECLARATIONS + ('\nspecify db 3,0dfh,3\n'
                                           'command db 45h,0,0,0,1,2,1,54h,0ffh\n')
                body = setup + '\n    jmp checked\n' + commands + ROUTINES + 'checked:\n'
                run_guest(args.dosbox.resolve(), args.nasm, directory, image, body, read_only=False,
                          extra_commands=('config -set cpu cpu_cycles=20000',))
                t0,p0,t1,p1 = unpack('<IHIH', (directory / 'CLOCK.BIN').read_bytes())
                elapsed = (65536*(t1-t0)+p0-p1)*1000/1193182
                # Loading the head misses the near-index ID on the first turn.
                turn = 200 if pio else 0
                assert 20.928 + turn <= elapsed < 21.1 + turn, (extension, pio, elapsed)
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
    '''
                prefix = supplied if pio else 512
                readback += '    mov di,buffer\n'
                if prefix:
                    readback += f'''
        mov cx,{prefix}
        mov al,66h
        repe scasb
        jne done
'''
                if prefix < 512:
                    readback += f'''
        mov cx,{512 - prefix}
        xor al,al
        repe scasb
        jne done
'''
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          program(readback, 'read_command db 46h,0,0,0,1,2,1,54h,0ffh\n'))
                assert Path(str(image) + '.overlay').read_bytes() == saved
                assert image.read_bytes() == original
                print(f'.{extension} {"PIO underrun" if pio else "DMA full write"}: '
                      f'result at {elapsed:.3f} ms; restart payload and CRC verified; '
                      'source unchanged', flush=True)
            for reset in (False, True):
                check_abort(args, directory, extension, reset)


if __name__ == "__main__":
    main()
