#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Select BIOS media rates independently of the previous drive's CCR value."""

import argparse
from pathlib import Path
from struct import pack_into
from tempfile import TemporaryDirectory

from run_floppy_read_deleted_probe import make_track
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest


def capture(rate, rpm, sectors):
    """Create native HD/ED geometry with only the tested first track populated."""
    cells = make_track(tuple((number, 0xfb, '', 0x77)
                             for number in range(1, sectors + 1)), 2)
    length = round(rate * 1000 * 60 / rpm / 8) * 2
    cells.extend(MFM[0][0x4e] * ((length - len(cells)) // 2))
    assert len(cells) == length
    image = bytearray(19 + 160 * 11)
    pack_into('<7sHBHHBI', image, 0, b'HXCMFM', 80, 2, rpm, rate, 7, 19)
    for slot in range(160):
        pack_into('<HBII', image, 19 + slot * 11, slot // 2, slot % 2,
                  len(cells) if slot == 0 else 0, len(image) if slot == 0 else 0)
    return image + cells


def checks(written, writing):
    """Alternate A/B reads and Verify, then write A and leave its rate selected."""
    code = ''
    for index, unit in enumerate((0, 1, 0, 1, 0)):
        value = 0x55 if written and unit == 0 else 0x77
        code += f'''
    mov byte [stage],{index + 1}
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    mov dx,{unit}
    int 13h
    jc done
    cmp ax,1
    jne done
    mov si,buffer
    mov cx,512
data_{index}:
    lodsb
    cmp al,{value}
    jne done
    loop data_{index}
    mov ax,0401h
    mov bx,buffer
    mov cx,1
    mov dx,{unit}
    int 13h
    jc done
    cmp ax,1
    jne done
'''
    if writing:
        code += '''
    mov byte [stage],6
    mov di,buffer
    mov cx,512
    mov al,55h
    rep stosb
    mov ax,0301h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    cmp ax,1
    jne done
'''
    return code


def main():
    """Check BIOS rate selection, drive switches and overlay restart persistence."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        folder = Path(temporary)
        control = folder / 'control.ima'
        control.write_bytes(b'\x77' * (360 * 1024))
        mount = (f'mount 1 "{control}" -t floppy -fs none -ro',)
        for rate, rpm, sectors in ((250, 300, 9), (300, 360, 9),
                                   (500, 300, 18), (500, 360, 15),
                                   (1000, 300, 36)):
            image = folder / f'bios-{rate}-{rpm}.mfm'
            original = capture(rate, rpm, sectors)
            image.write_bytes(original)
            for written, writing in ((False, True), (True, False)):
                run_guest(args.dosbox.resolve(), args.nasm, folder, image,
                          checks(written, writing), extra_commands=mount,
                          read_only=False)
            assert image.read_bytes() == original
            assert Path(str(image) + '.overlay').exists()
            print(f'{rate} kbps/{rpm} RPM: BIOS reads, Verify, drive switches, '
                  'write/restart and original-image guard passed', flush=True)


if __name__ == '__main__':
    main()
