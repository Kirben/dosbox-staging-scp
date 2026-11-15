#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check memory shift side effects with VGA XOR latches."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_transcopy_layout_probe import run_guest


def indexed(port, register, datum):
    """Write a VGA indexed register."""
    return f'mov dx,{port}\nmov al,{register}\nout dx,al\ninc dx\nmov al,{datum}\nout dx,al\n'

body = '''
    mov ax,12h
    int 10h
    push cs
    pop ds
    mov ax,0a000h
    mov es,ax
'''
body += indexed(0x3c4, 2, 1)
for index, value in ((1, 0), (3, 0), (4, 0), (5, 0), (8, 255)):
    body += indexed(0x3ce, index, value)
body += 'mov byte [es:0],1\n' + indexed(0x3ce, 3, 0x18) + '''
    mov cl,3
    shl byte [es:0],cl
    mov al,[es:0]
    mov [saved],al
    mov al,1
    shl al,cl
    mov [saved+1],al
    mov byte [ram],1
    shl byte [ram],cl
    mov al,[ram]
    mov [saved+2],al
    mov ax,3
    int 10h
    push cs
    pop ds
    mov dx,output
    xor cx,cx
    mov ah,3ch
    int 21h
    mov bx,ax
    mov dx,saved
    mov cx,3
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
    jmp checked
saved times 3 db 0
ram db 0
output db 'SHIFT.BIN',0
checked:
'''
def main():
    """Check processor controls against single-write VGA semantics."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', type=Path, required=True)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory(prefix='8088-vga-shift-') as temporary:
        folder = Path(temporary)
        image = folder / 'unused.img'
        image.write_bytes(bytes(360 * 1024))
        for width in ('byte', 'word'):
            code = body.replace('byte [es:0]', f'{width} [es:0]')
            if width == 'word':
                code = code.replace('word [es:0],1', 'word [es:0],101h')
            for cpu in ('386', '8088'):
                run_guest(args.dosbox, args.nasm, folder, image, code,
                          extra_commands=(f'config -set cpu cputype={cpu}',
                                          'config -set cpu core=normal'))
                data = (folder / 'SHIFT.BIN').read_bytes()
                assert data == bytes((9, 8, 8)), (width, cpu, data.hex())
                print(f'{cpu} {width}: VGA, register and RAM controls passed')


if __name__ == '__main__':
    main()
