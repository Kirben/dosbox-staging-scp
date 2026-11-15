#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep DOS absolute I/O working with and without a BIOS floppy attachment."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_fat_floppy_cache_probe import fat12
from run_hxc_mfm_probe import encode
from run_scp_attachment_probe import encode_scp
from run_transcopy_attachment_probe import encode_transcopy
from run_transcopy_layout_probe import run_guest


def checks(unit, marker, writing, protected):
    """Read a data sector and optionally overwrite it with INT 26h."""
    code = f'''
    mov al,{unit}
    mov bx,buffer
    mov cx,1
    mov dx,6
    int 25h
    pushf
    pop di
    popf
    test di,1
    jnz done
    test ax,ax
    jnz done
    cmp byte [buffer],{marker}
    jne done
'''
    if writing:
        code += f'''
    mov byte [buffer],55h
    mov al,{unit}
    mov bx,buffer
    mov cx,1
    mov dx,6
    int 26h
    pushf
    pop di
    popf
    test di,1
    {'jz' if protected else 'jnz'} done
'''
    return code


def main():
    """Compare A and E, persistent writes, and explicitly read-only controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        folder = Path(temporary)
        for extension, encoder in (('img', bytes), ('ima', bytes), ('scp', encode_scp),
                                   ('mfm', encode), ('86f', encode_86f),
                                   ('tc', encode_transcopy)):
            original = encoder(fat12())
            for letter, unit in (('a', 0), ('e', 4)):
                image = folder / f'absolute-{letter}.{extension}'
                image.write_bytes(original)
                for writing, protected, marker in ((True, True, 0x31),
                                                    (True, False, 0x31),
                                                    (False, True, 0x55)):
                    mount = f'mount {letter} "{image}" -t floppy' + (
                        ' -ro' if protected else '')
                    run_guest(args.dosbox.resolve(), args.nasm, folder, image,
                              checks(unit, marker, writing, protected),
                              extra_commands=('mount -u 0', mount))
                if extension not in ('img', 'ima'):
                    assert image.read_bytes() == original
                print(f'{extension} {letter.upper()}: INT25/26, read-only and restart passed',
                      flush=True)


if __name__ == '__main__':
    main()
