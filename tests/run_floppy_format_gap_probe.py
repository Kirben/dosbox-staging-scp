#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure GPL-dependent FM/MFM sector positions before and after restart."""

import argparse
from pathlib import Path
from struct import unpack
from tempfile import TemporaryDirectory

from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS
from run_floppy_transfer_edges_probe import DMA, one_sector
from run_floppy_write_deleted_probe import routines, setup
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def format_checks(pio, fm, gap):
    """Supply duplicate and nonsequential IDs, checking TC/IRQ and byte counts."""
    size_code = 0 if fm else 2
    code = DMA.replace('mov al,0ffh\n    out 05h,al', 'mov al,11\n    out 05h,al')
    code = code.replace('mov al,1\n    out 05h,al', 'xor al,al\n    out 05h,al')
    for offset, sector in enumerate((5, 5, 9)):
        code += f'mov dword [buffer+{offset * 4}],{(size_code << 24) | (sector << 16)}\n'
    code += 'mov al,4ah\nout 0bh,al\n'
    if pio:
        code += 'mov al,6\nout 0ah,al\n'
    code += f'''
    mov byte [command],{0x0d if fm else 0x4d}
    mov byte [command+1],0
    mov byte [command+2],{size_code}
    mov byte [command+3],3
    mov byte [command+4],{gap}
    mov byte [command+5],0e5h
    mov cx,6
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    cmp word [io_count],{12 if pio else 0}
    jne done
    in al,08h
    and al,4
    cmp al,{0 if pio else 4}
    jne done
    inc byte [stage]
'''
    return code


def read_ids(fm, sectors=(5, 5, 9, 5)):
    """Timestamp four successive Read ID completions, including index wrap."""
    checks = CLOCK_SETUP + 'mov di,timestamps\ncall stamp\n'
    for index, sector in enumerate(sectors):
        checks += f'''
    mov byte [command],{0x0a if fm else 0x4a}
    mov byte [command+1],0
    mov cx,2
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
'''
        if sector is None:
            checks += f'''
    cmp byte [ids+5],5
    je valid_legacy_{index}
    cmp byte [ids+5],9
    jne done
valid_legacy_{index}:
'''
        else:
            checks += f'cmp byte [ids+5],{sector}\njne done\n'
        checks += f'''
    mov di,timestamps+{(index + 1) * 6}
    call stamp
    inc byte [stage]
'''
    return checks + '''
    mov dx,filename
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc done
    mov bx,ax
    mov dx,timestamps
    mov cx,30
    mov ah,40h
    int 21h
    cmp ax,30
    jne done
    mov ah,3eh
    int 21h
'''


def elapsed_times(directory):
    """Convert BIOS tick/PIT pairs into milliseconds from the first stamp."""
    values = unpack('<' + 'IH' * 5, (directory / 'CLOCK.BIN').read_bytes())
    start = values[0] * 65536 - values[1]
    return [(values[i] * 65536 - values[i + 1] - start) * 1000 / 1193182
            for i in range(0, 10, 2)]


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check(args, directory, extension, pio, fm, gap):
    """The measured spacing follows bytes and GPL, not SC or logical R."""
    image = directory / f'gap-{pio}-{fm}-{gap}.{extension}'
    original = encode_tracks(extension, [one_sector(2)])
    image.write_bytes(original)
    functions = routines(pio).replace('    mov cx,9\n', '')
    functions = functions.replace('checked:\n',
                                  DECLARATIONS.replace('timestamps times 12',
                                                       'timestamps times 30') + 'checked:\n')
    measured = []
    for writing in (True, False):
        checks = setup(pio)
        if writing:
            checks += format_checks(pio, fm, gap)
        checks += read_ids(fm)
        run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                  checks + functions, read_only=not writing,
                  machine='pcjr' if pio else 'svga_s3',
                  extra_commands=('config -set cpu cpu_cycles=20000',))
        times = elapsed_times(directory)
        stride = (33 + 128 if fm else 62 + 512) + gap
        expected = stride * (0.064 if fm else 0.032)
        for interval in (times[2] - times[1], times[3] - times[2]):
            assert abs(interval - expected) < 0.4, (image.name, times, expected)
        assert abs(times[4] - times[1] - 200) < 0.4, (image.name, times)
        if writing:
            first = 86 * 0.064 if fm else 168 * 0.032
            assert abs(times[1] - first) < 0.4, (image.name, times, first)
        measured.append(times[2] - times[1])
        overlay = Path(str(image) + '.overlay')
        current = overlay.read_bytes()
        assert current[6] == 11 and current[25] == gap
        if writing:
            saved = current
        else:
            assert current == saved
    assert image.read_bytes() == original
    if not pio and gap == 0:
        check_legacy(args, directory, image, fm, functions, saved)
    print(f'{extension} {"FM" if fm else "MFM"} {"PIO" if pio else "DMA"} '
          f'GPL={gap}: spacing {measured[0]:.3f}/{measured[1]:.3f} ms, '
          'duplicate IDs, positions and restart passed', flush=True)


# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def check_legacy(args, directory, image, fm, functions, saved):
    """Version 10 must retain its historical evenly spaced layout."""
    overlay = Path(str(image) + '.overlay')
    legacy = bytearray(saved)
    legacy[6] = 10
    legacy[24] &= 1
    del legacy[25]
    overlay.write_bytes(legacy)
    # Initial head-load timing can put the first command on either copy
    # of ID 5. Every following physical interval must still be one third.
    checks = setup(False) + read_ids(fm, (None,) * 4)
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              checks + functions,
              extra_commands=('config -set cpu cpu_cycles=20000',))
    times = elapsed_times(directory)
    for index in range(1, 4):
        assert abs(times[index + 1] - times[index] - 200 / 3) < 0.4, times
    assert overlay.read_bytes() == legacy
    overlay.write_bytes(saved)


def check_capacity(args, directory, extension):
    """A large GPL exhausts the turn without overwriting its first IDs."""
    image = directory / f'capacity.{extension}'
    original = encode_tracks(extension, [one_sector(2)])
    image.write_bytes(original)
    checks = format_checks(False, False, 240)
    checks = checks.replace('mov al,11\n', 'mov al,35\n')
    checks = checks.replace('mov byte [command+3],3', 'mov byte [command+3],9')
    checks = checks.replace('cmp al,4\n', 'cmp al,0\n')
    ids = ''.join(f'mov dword [buffer+{i * 4}],{(2 << 24) | ((i + 1) << 16)}\n'
                  for i in range(9))
    checks = checks.replace('    mov byte [command],77', ids + '    mov byte [command],77')
    functions = routines(False).replace('    mov cx,9\n', '')
    functions = functions.replace('checked:\n',
                                  DECLARATIONS.replace('timestamps times 12',
                                                       'timestamps times 30') + 'checked:\n')
    for writing in (True, False):
        body = setup(False) + (checks if writing else '') + read_ids(False, (1, 2, 3, 4))
        run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                  body + functions, read_only=not writing,
                  extra_commands=('config -set cpu cpu_cycles=20000',))
        times = elapsed_times(directory)
        for index in range(1, 4):
            assert abs(times[index + 1] - times[index] - 26.048) < 0.4, times
        overlay = Path(str(image) + '.overlay').read_bytes()
        # Eight ID fields fit; the eighth data field is clipped at index.
        assert overlay[17] == 0xfe and overlay[19] == 8 and overlay[21] == 9
    assert image.read_bytes() == original
    print(f'{extension}: large GPL stops at index, retains initial IDs and replays', flush=True)


def main():
    """Check all writable track backends using DMA and PCjr non-DMA."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    parser.add_argument('--formats', nargs='+', default=['scp', 'mfm', '86f', 'tc'])
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for extension in args.formats:
            for pio in (False, True):
                for fm in (False, True):
                    for gap in (0, 0x54, 0xf0):
                        check(args, directory, extension, pio, fm, gap)
            check_capacity(args, directory, extension)


if __name__ == '__main__':
    main()
