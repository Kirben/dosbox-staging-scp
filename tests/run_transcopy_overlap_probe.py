#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check DMA/PIO Read Track through TC's recorded index continuation."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_long_read_track_probe import CHECKS, encoded_cells, make_image
from run_transcopy_layout_probe import run_guest


def decoded_data(bits):
    """Independently decode a command-sized field on the supplied ring."""
    sync = f'{0x448944894489:048b}'
    cursor = 0
    doubled = bits * 2
    while True:
        mark = doubled.index(sync, cursor)
        if int(doubled[mark + 49:mark + 64:2], 2) == 0xfb:
            start = mark + 64
            break
        cursor = mark + 1
    return bytes(int(''.join(bits[(start + byte * 16 + bit * 2 + 1) % len(bits)]
                             for bit in range(8)), 2) for byte in range(8192))


def packed_cells(bits):
    """Pack captured cells without rounding the physical turn itself."""
    return bytes(int((bits[i:i + 8] + '0000000')[:8], 2)
                 for i in range(0, len(bits), 8))


def fixture(recorded):
    """Capture one rounded turn plus an actual second-turn prefix."""
    disk = bytes([0x31]) * (360 * 1024)
    cells = encoded_cells(disk)
    physical = ''.join(f'{byte:08b}' for byte in cells)[:-3]
    iam = physical.index(f'{0x5224522452245552:064b}')
    start = iam - 64
    ring = physical[start:] + physical[:start]
    declared_bytes = (len(ring) + 7) // 8
    stream = (ring * 2)[:declared_bytes * 8 + 2048]
    # A capture's initial acquisition is not the next revolution's prefix.
    stream = '1' * 16 + stream[16:]
    first = packed_cells(stream[:declared_bytes * 8])
    image = make_image(first, 'tc')
    image.extend(packed_cells(stream[declared_bytes * 8:]) if recorded else bytes(256))
    expected = decoded_data(ring if recorded else stream[:declared_bytes * 8])
    assert decoded_data(ring) != decoded_data(stream[:declared_bytes * 8])
    return image, expected


def main():
    """Compare full data, CRC status, terminal count, IRQ6 and a guard byte."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', type=Path, required=True)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory(prefix='tc-overlap-') as temporary:
        folder = Path(temporary)
        for recorded in (False, True):
            original, expected = fixture(recorded)
            image = folder / f'overlap-{recorded}.tc'
            image.write_bytes(original)
            for pio in (False, True):
                options = '%define LENGTH 8192\n%define BLOCKS 1\n'
                if pio:
                    options += '%define PIO\n'
                run_guest(args.dosbox.resolve(), args.nasm, folder, image,
                          options + CHECKS, expected,
                          machine='pcjr' if pio else 'svga_s3')
                assert image.read_bytes() == original
                assert not Path(str(image) + '.overlay').exists()
                print(f'TC overlap={recorded}, PIO={pio}: data/status/IRQ/guard passed',
                      flush=True)


if __name__ == '__main__':
    main()
