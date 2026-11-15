#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise original BIOS-style DMA Verify without memory or FIFO data strobes."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_transfer_edges_probe import DMA, RESET, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def checks(mode, bad_crc):
    """Check status, IRQ6, terminal count, auto-init, address and untouched RAM."""
    verify = mode in (0x42, 0x52)
    auto_init = mode == 0x52
    code = RESET + DMA.replace('mov al,46h\n    out 0bh,al',
                               f'mov al,{mode}\n    out 0bh,al')
    code += f'''
    in al,08h
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    mov byte [stage],5
    cmp byte [ids],{0x40 if bad_crc else 0}
    jne done
    mov byte [stage],6
    cmp byte [ids+1],{0x20 if bad_crc else 0}
    jne done
    mov byte [stage],7
    cmp byte [ids+2],{0x20 if bad_crc else 0}
    jne done
    mov byte [stage],2
    in al,08h
    test al,4
    jz done
    xor al,al
    out 0ch,al
    in al,05h
    cmp al,0ffh
    jne done
    in al,05h
    cmp al,{1 if auto_init else 0xff}
    jne done
    mov byte [stage],3
    xor eax,eax
    mov ax,ds
    shl eax,4
    add eax,buffer
    {'add eax,512' if not auto_init else ''}
    mov bx,ax
    xor al,al
    out 0ch,al
    in al,04h
    cmp al,bl
    jne done
    in al,04h
    cmp al,bh
    jne done
    mov byte [stage],4
    mov si,buffer
    mov cx,512
check_buffer:
    lodsb
    cmp al,{0xa5 if verify else 0x77}
    jne done
    loop check_buffer
'''
    return program(code, 'command db 46h,0,0,0,1,2,1,54h,0ffh\n')


def main():
    """Compare normal reads and count-only Verify cycles on every backend."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        folder = Path(temporary)
        for extension in ('img', 'ima', 'scp', 'mfm', '86f', 'tc'):
            for bad_crc in ((False,) if extension in ('img', 'ima') else (False, True)):
                image = folder / f'verify-{bad_crc}.{extension}'
                original = (b'\x77' * (360 * 1024) if extension in ('img', 'ima') else
                            encode_tracks(extension, [one_sector(2, bad_crc)]))
                image.write_bytes(original)
                for mode in (0x46, 0x42, 0x52):
                    print(f'{extension}: mode={mode:02x} bad_crc={bad_crc}', flush=True)
                    run_guest(args.dosbox.resolve(), args.nasm, folder, image,
                              checks(mode, bad_crc))
                assert image.read_bytes() == original
            print(f'{extension}: DMA Verify, CRC, TC, auto-init and RAM guards passed',
                  flush=True)


if __name__ == '__main__':
    main()
