#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check recorded TC Read ID timing, raw fallback and overlay persistence."""

import argparse
from hashlib import sha256
from pathlib import Path
import re
from statistics import median
from struct import pack_into, unpack_from
import subprocess
from tempfile import TemporaryDirectory

from run_floppy_id_search_probe import wait_microseconds
from run_floppy_transfer_edges_probe import RESET, DMA, program
from run_hxc_mfm_probe import encode_track, launch
from run_transcopy_layout_probe import run_guest


def raw_intervals(cells):
    """Locate IDs independently of the image reader."""
    bits = ''.join(f'{byte:08b}' for byte in cells)
    positions = {}
    for match in re.finditer('0100010010001001' * 3, bits):
        data = bytes(int(bits[i + 1:i + 16:2], 2)
                     for i in range(match.end(), match.end() + 80, 16))
        if data[0] == 0xfe:
            positions[data[3]] = match.start()
    assert set(positions) == set(range(1, 10))
    return {sector: (positions[sector] - positions[sector - 1 or 9])
            % len(bits) / 500 for sector in range(1, 10)}


def fixture(recorded):
    """Give C51/H1 a distinct recorded R2 interval without changing cells."""
    disk = bytes(52 * 2 * 9 * 512)
    image = bytearray(0x4000)
    image[:2] = b'\x5a\xa5'
    image[0x100:0x105] = bytes((7, 0, 51, 2, 1))
    expected = {}
    for cylinder in (0, 51):
        for head in (0, 1):
            track = cylinder * 2 + head
            cells = encode_track(disk, track)
            skew = 10000 if head else 0
            pack_into('<H', image, 0x105 + track * 2, skew)
            pack_into('>H', image, 0x305 + track * 2, len(image) // 256)
            pack_into('<H', image, 0x505 + track * 2, len(cells))
            image.extend(cells)
            image.extend(bytes(-len(image) % 256))
            if cylinder != 51:
                continue
            expected[head] = raw_intervals(cells)
            if recorded:
                intervals = [1000 + skew, 4000 + head * 300] + [4000] * 7
                intervals.append(40000 + skew - sum(intervals))
                for i, ticks in enumerate(intervals):
                    pack_into('<H', image, 0x905 + track * 32 + i * 2, ticks)
                expected[head] = {sector: 20.0 for sector in range(1, 10)}
                expected[head][2] = 20 + head * 1.5
                expected[head][1] = 40 - head * 1.5
    return image, expected


def measure(args, folder, image, expected):
    """Read both heads and compare independent per-ID PIT intervals."""
    for name in ('TIMES.BIN', 'RESULT.BIN', 'SETUP.BIN'):
        (folder / name).unlink(missing_ok=True)
    subprocess.run([args.nasm, '-f', 'bin',
                    Path(__file__).with_name('transcopy_timing_probe.asm'),
                    '-o', folder / 'PROBE.COM'], check=True)
    command, environment = launch(args.dosbox.resolve(), folder, 'timing', [
        f'mount c "{folder}"', f'mount 0 "{image}" -t floppy -fs none -ro',
        'config -set cpu core=normal', 'config -set cpu cpu_cycles=3000',
        'c:', 'PROBE.COM', 'exit',
    ])
    subprocess.run(command, cwd=folder, env=environment, capture_output=True,
                   text=True, timeout=30, check=True)
    assert (folder / 'RESULT.BIN').read_bytes() == b'\0'
    assert (folder / 'SETUP.BIN').read_bytes() == bytes((0x20, 51))
    data = (folder / 'TIMES.BIN').read_bytes()
    assert len(data) == 48 * 19
    check_intervals(data, expected)


def check_intervals(data, expected):
    """Compare completed Read ID intervals for each head."""
    for head in (0, 1):
        previous = None
        intervals = {}
        for i in range(head * 24, (head + 1) * 24):
            row = data[i * 19:(i + 1) * 19]
            result = row[6:13]
            assert result[:3] == bytes((head << 2, 0, 0)), result.hex()
            assert result[3:5] == bytes((51, head)) and result[6] == 2, result.hex()
            tick, pit = unpack_from('<IH', row, 13)
            if previous:
                duration = ((tick - previous[0]) * 65536 + previous[1] - pit) / 1193.182
                intervals.setdefault(result[5], []).append(duration)
            previous = tick, pit
        for sector, duration in expected[head].items():
            assert sector in intervals, (head, sector, intervals)
            observed = median(intervals[sector])
            assert abs(observed - duration) < 0.02, (head, sector, observed, duration)


def write_sector(args, folder, image):
    """A payload overlay must not reset captured address-mark positions."""
    body = '''
    cld
    mov di,buffer
    mov al,0a6h
    mov cx,512
    rep stosb
    mov ax,0301h
    mov bx,buffer
    mov cx,3302h
    xor dx,dx
    int 13h
    jc done
    mov ax,0201h
    int 13h
    jc done
    mov di,buffer
    mov cx,512
    mov al,0a6h
    repe scasb
    jne done
'''
    run_guest(args.dosbox.resolve(), args.nasm, folder, image, body, read_only=False)


def interrupt_format(args, folder, image):
    """Preserve original timing on fields beyond an interrupted write arc."""
    body = '''
    mov ax,0201h
    mov bx,buffer
    mov cx,3301h
    xor dx,dx
    int 13h
    jc done
    mov dx,3f2h
    xor al,al
    out dx,al
'''
    body += RESET + DMA.replace('mov al,0ffh\n    out 05h,al',
                                'mov al,3\n    out 05h,al').replace(
                                    'mov al,1\n    out 05h,al',
                                    'xor al,al\n    out 05h,al')
    body += '''
    mov dword [buffer],02090033h
    mov al,4ah
    out 0bh,al
    mov si,format_command
    mov cx,6
    call send_command
'''
    body += wait_microseconds(250000)
    body += 'mov dx,3f2h\nmov al,18h\nout dx,al\nmov al,1ch\nout dx,al\n'
    run_guest(args.dosbox.resolve(), args.nasm, folder, image,
              program(body, 'format_command db 4dh,0,2,1,54h,0e5h\n'), read_only=False)


def main():
    """Check raw fallback and recorded times through fresh emulator instances."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', type=Path, required=True)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory(prefix='tc-mark-timing-') as temporary:
        folder = Path(temporary)
        for recorded in (False, True):
            original, expected = fixture(recorded)
            image = folder / f'timing-{recorded}.tc'
            image.write_bytes(original)
            fingerprint = sha256(original).hexdigest()
            measure(args, folder, image, expected)
            write_sector(args, folder, image)
            overlay = Path(str(image) + '.overlay')
            saved = overlay.read_bytes()
            measure(args, folder, image, expected)
            assert overlay.read_bytes() == saved
            if recorded:
                interrupt_format(args, folder, image)
                assert overlay.read_bytes() != saved
                saved = overlay.read_bytes()
                surviving = {0: {sector: 20 for sector in range(5, 9)}, 1: expected[1]}
                measure(args, folder, image, surviving)
                assert overlay.read_bytes() == saved
            assert sha256(image.read_bytes()).hexdigest() == fingerprint
            print(f'TC recorded={recorded}: C51 timing, skew and overlay restart passed',
                  flush=True)


if __name__ == '__main__':
    main()
