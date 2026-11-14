#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Persist FM clock marks, CRCs and formats without modifying captures."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_fm_probe import fm_byte, fm_image, fm_track
from run_floppy_read_deleted_probe import make_track
from run_floppy_transfer_edges_probe import DMA, one_sector
from run_floppy_write_deleted_probe import setup, transfer, routines
from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest


# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def fm_transfer(opcode, marker, pio, status_2=0, rejected=0, size_code=2):
    """Keep the existing wire/DMA/PIO assertions, selecting FM in the command."""
    code = transfer(opcode, marker, pio, status_2, rejected, size_code)
    return code.replace(f'mov byte [command],{opcode}',
                        f'mov byte [command],{opcode & ~0x40}')


# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def run(args, directory, image, pio, checks, writable=False, selector=2):
    """Use a fresh emulator for every readback, including the PCjr PIO path."""
    wire = directory / 'WIRE.BIN'
    wire.unlink(missing_ok=True)
    functions = routines(pio).replace(' done', ' failed').replace('checked:', '''
failed:
    mov dx,wire_name
    xor cx,cx
    mov ah,3ch
    int 21h
    jc done
    mov bx,ax
    mov dx,ids
    mov cx,9
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
    jmp done
wire_name db 'WIRE.BIN',0
checked:
''')
    try:
        run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                  setup(pio) + f'mov dx,3f7h\nmov al,{selector}\nout dx,al\n'
                  + checks.replace(' done', ' failed') + functions, read_only=not writable,
                  machine='pcjr' if pio else 'svga_s3',
                  extra_commands=('config -set cpu cpu_cycles=20000',))
    except RuntimeError as error:
        result = wire.read_bytes().hex() if wire.exists() else 'unavailable'
        raise RuntimeError(f'{error}\nST0/ST1/ST2/CHRN/PIO count: {result}') from error


def check_writes(args, directory, extension, pio, size_code):
    """Rewrite one duplicate, retain the other, and replay F8/FB with valid CRCs."""
    cells = fm_byte(0xff) * 64 + fm_track(size_code)
    cells = cells[:6250]
    image = directory / f'write-{pio}-{size_code}.{extension}'
    original = fm_image(extension, cells)
    image.write_bytes(original)
    run(args, directory, image, pio,
        fm_transfer(0x45, 0x55, pio, size_code=size_code), True)
    run(args, directory, image, pio,
        fm_transfer(0x49, 0x66, pio, size_code=size_code), True)
    overlay = Path(str(image) + '.overlay')
    saved = overlay.read_bytes()
    assert saved[6] == 10
    checks = fm_transfer(0x46, 0x66, pio, 0x40, size_code=size_code)
    checks += fm_transfer(0x46, 0x22, pio, size_code=size_code)
    checks += fm_transfer(0x4c, 0x66, pio, size_code=size_code)
    checks += fm_transfer(0x42, 0x66, pio, 0x40, size_code=size_code)
    run(args, directory, image, pio, checks)
    assert overlay.read_bytes() == saved
    run(args, directory, image, pio,
        fm_transfer(0x45, 0x88, pio, size_code=size_code), True)
    run(args, directory, image, pio,
        fm_transfer(0x46, 0x88, pio, size_code=size_code)
        + fm_transfer(0x46, 0x22, pio, size_code=size_code))
    saved = overlay.read_bytes()
    run(args, directory, image, pio,
        fm_transfer(0x45, 0x99, pio, rejected=2, size_code=size_code))
    assert overlay.read_bytes() == saved and image.read_bytes() == original
    print(f'{extension} FM {"PIO" if pio else "DMA"} N={size_code}: '
          'duplicate selection, F8/FB, CRC, TC/IRQ, guards and restart passed', flush=True)


def check_mixed(args, directory, extension):
    """Retain a faster MFM field beside FM fields with the same CHRN."""
    fm = fm_byte(0xff) * 64 + fm_track(2)
    bits = ''.join(f'{byte:08b}' for byte in fm[:3000])
    expanded = ''.join('0' + bit for bit in bits)
    cells = bytearray(int(expanded[i:i + 8], 2) for i in range(0, len(expanded), 8))
    cells.extend(make_track(((1, 0xfb, '', 0x33),), 2)[:1600])
    cells.extend(MFM[0][0x4e] * ((12500 - len(cells)) // 2))
    image = directory / f'mixed.{extension}'
    original = encode_tracks(extension, [cells])
    image.write_bytes(original)
    run(args, directory, image, False, fm_transfer(0x45, 0x66, False), True)
    run(args, directory, image, False,
        transfer(0x46, 0x33, False) + fm_transfer(0x46, 0x66, False))
    run(args, directory, image, False, transfer(0x45, 0x44, False), True)
    run(args, directory, image, False,
        transfer(0x46, 0x44, False) + fm_transfer(0x46, 0x66, False))
    assert image.read_bytes() == original
    print(f'{extension}: mixed FM/MFM shared CHRN writes retain the other encoding', flush=True)


def format_command(pio, size_code=0, partial=False, rejected=0):
    """Format two IDs or stop DMA after one, checking IRQ and terminal count."""
    count = 4 if partial else 8
    code = DMA.replace('mov al,0ffh\n    out 05h,al',
                       f'mov al,{count - 1}\n    out 05h,al').replace(
                           'mov al,1\n    out 05h,al', 'xor al,al\n    out 05h,al')
    code += f'mov dword [buffer],{(size_code << 24) | (1 << 16)}\n'
    code += f'mov dword [buffer+4],{(size_code << 24) | (2 << 16)}\n'
    code += 'mov al,4ah\nout 0bh,al\n'
    if pio:
        code += 'mov al,6\nout 0ah,al\n'
    port = 0xf5 if pio else 0x3f5
    code += f'''
    in al,08h
    push es
    xor ax,ax
    mov es,ax
    and byte [es:43eh],7fh
    pop es
    mov dx,{port}
    mov al,0dh
    out dx,al
    xor al,al
    out dx,al
    mov al,{size_code}
    out dx,al
    mov al,2
    out dx,al
    mov al,1bh
    out dx,al
    mov al,0e5h
    out dx,al
    call wait_result
    cmp byte [ids+1],{rejected or (0x10 if partial else 0)}
    jne done
    cmp byte [ids+2],0
    jne done
    cmp word [io_count],{count if pio and not rejected else 0}
    jne done
    in al,08h
    and al,4
    cmp al,{4 if not pio and not rejected else 0}
    jne done
    push es
    xor ax,ax
    mov es,ax
    test byte [es:43eh],80h
    pop es
    {'jnz' if pio else 'jz'} done
    mov di,buffer+8
    mov cx,16
    mov al,0a5h
    repe scasb
    jne done
    inc byte [stage]
'''
    return code


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check_format(args, directory, extension, pio, rate, selector):
    """Convert MFM to FM and reopen the saved clock and encoding metadata."""
    image = directory / f'format-{pio}-{rate}.{extension}'
    original = encode_tracks(extension, [one_sector(2)])
    image.write_bytes(original)
    run(args, directory, image, pio, format_command(pio), True, selector)
    overlay = Path(str(image) + '.overlay')
    saved = overlay.read_bytes()
    assert saved[6] == 10 and saved[24] == 1
    checks = fm_transfer(0x46, 0xe5, pio, size_code=0)
    checks += fm_transfer(0x42, 0xe5, pio, size_code=0)
    checks += 'mov byte [command+4],2\nmov byte [command+6],2\n'
    checks += fm_transfer(0x46, 0xe5, pio, size_code=0).replace(
        'cmp byte [ids+5],1', f'cmp byte [ids+5],{2 if pio else 1}')
    checks += 'mov byte [command+4],1\nmov byte [command+6],1\n'
    checks += transfer(0x46, 0xa5, pio, rejected=1, size_code=0)
    run(args, directory, image, pio, checks, selector=selector)
    assert overlay.read_bytes() == saved
    run(args, directory, image, pio,
        fm_transfer(0x49, 0x66, pio, size_code=0), True, selector)
    run(args, directory, image, pio,
        fm_transfer(0x4c, 0x66, pio, size_code=0), selector=selector)
    saved = overlay.read_bytes()
    run(args, directory, image, pio, format_command(pio, rejected=2), selector=selector)
    assert overlay.read_bytes() == saved and image.read_bytes() == original
    if not pio and rate == 250:
        damaged_encoding = bytearray(saved)
        damaged_encoding[24] = 2
        damaged_mark = bytearray(saved)
        damaged_mark[-129] = 4
        missing_fm = bytearray(saved)
        missing_fm[-129] = 0
        for damaged in (damaged_encoding, damaged_mark, missing_fm, saved[:-1]):
            overlay.write_bytes(damaged)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image, '''
    mov ax,0800h
    xor dx,dx
    int 13h
    jnc done
''')
            assert overlay.read_bytes() == damaged
        overlay.write_bytes(saved)
    print(f'{extension}: FM format {rate} kbps {"PIO" if pio else "DMA"}, '
          'N=0, CRC, MFM rejection, deleted rewrite and restart passed', flush=True)


def check_partial(args, directory, extension):
    """Keep the MFM suffix of an interrupted FM format at a slower clock."""
    # Put an independent MFM field beyond the half-track format cutoff.
    prefix = bytearray(MFM[0][0x4e] * 3500)
    prefix.extend(make_track(((1, 0xfb, '', 0x77),), 2)[:1600])
    prefix.extend(MFM[0][0x4e] * ((12500 - len(prefix)) // 2))
    image = directory / f'partial.{extension}'
    original = encode_tracks(extension, [prefix])
    image.write_bytes(original)
    run(args, directory, image, False, format_command(False, partial=True), True)
    run(args, directory, image, False,
        fm_transfer(0x46, 0xe5, False, size_code=0) + transfer(0x46, 0x77, False))
    run(args, directory, image, False,
        fm_transfer(0x45, 0x55, False, size_code=0), True)
    run(args, directory, image, False,
        fm_transfer(0x46, 0x55, False, size_code=0) + transfer(0x46, 0x77, False))
    run(args, directory, image, False, transfer(0x45, 0x88, False), True)
    run(args, directory, image, False,
        fm_transfer(0x46, 0x55, False, size_code=0) + transfer(0x46, 0x88, False))
    # Formatting again must preserve the canonical faster suffix, even
    # when the first visible sector belongs to the slower FM clock bank.
    run(args, directory, image, False, format_command(False, partial=True), True)
    run(args, directory, image, False,
        fm_transfer(0x46, 0xe5, False, size_code=0) + transfer(0x46, 0x88, False))
    assert image.read_bytes() == original
    print(f'{extension}: interrupted FM prefix and faster MFM suffix survive restart', flush=True)


def check_raw(args, directory, extension, formatting):
    """Sector-only files cannot retain FM encoding and must stay unchanged."""
    image = directory / f'raw.{extension}'
    original = bytes(360 * 1024)
    image.write_bytes(original)
    checks = (format_command(False, rejected=4) if formatting else
              fm_transfer(0x45, 0x55, False, rejected=4)
              + fm_transfer(0x49, 0x66, False, rejected=4))
    run(args, directory, image, False, checks, True)
    assert image.read_bytes() == original


def main():
    """Run FM writes and formatting independently for useful CTest timeouts."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    parser.add_argument('--format', action='store_true')
    parser.add_argument('--formats', nargs='+', default=['scp', 'mfm', '86f', 'tc'])
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for extension in args.formats:
            if args.format:
                for pio in (False, True):
                    for rate, selector in ((250, 2), (300, 1), (500, 0)):
                        check_format(args, directory, extension, pio, rate, selector)
                check_partial(args, directory, extension)
            else:
                for pio in (False, True):
                    for size_code in (0, 1, 2, 3):
                        check_writes(args, directory, extension, pio, size_code)
                check_mixed(args, directory, extension)
        for extension in ('img', 'ima'):
            check_raw(args, directory, extension, args.format)


if __name__ == '__main__':
    main()
