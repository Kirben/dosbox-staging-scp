#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write alternate-rate MFM fields without changing other fields or the capture."""

import argparse
from pathlib import Path
from struct import pack_into
from tempfile import TemporaryDirectory

from run_floppy_fm_write_probe import run
from run_floppy_read_deleted_probe import make_track
from run_floppy_separator_probe import capture
from run_floppy_write_deleted_probe import transfer
from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import MFM


def fixture(extension):
    """Put duplicate 250-kbps IDs beside a 500-kbps sector in a 200-ms turn."""
    slow = make_track(((1, 0xfb, '', 0x11), (1, 0xfb, '', 0x77)), 2)[:3000]
    bits = ''.join(f'{byte:08b}' for byte in slow)
    expanded = ''.join('0' + bit for bit in bits)
    cells = bytearray(int(expanded[i:i + 8], 2) for i in range(0, len(expanded), 8))
    cells.extend(make_track(((2, 0xfb, '', 0x33),), 2)[:1600])
    cells.extend(MFM[0][0x4e] * ((25000 - len(cells)) // 2))
    if extension == 'scp':
        return capture(cells, 40)
    image = encode_tracks(extension, [cells])
    if extension == 'tc':
        image[0x100] = 2
    elif extension == '86f':
        pack_into('<H', image, 1032, 0x08)
    return image


def fast_transfer(opcode, marker):
    """Select the fast sector's R/EOT while retaining the common wire checks."""
    return ('mov byte [command+4],2\nmov byte [command+6],2\n' +
            transfer(opcode, marker, False))


def main():
    """Rewrite each clock grid, restart, and verify both duplicate payloads."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        folder = Path(temporary)
        for extension in ('mfm', 'scp', '86f', 'tc'):
            image = folder / f'mixed.{extension}'
            original = fixture(extension)
            image.write_bytes(original)
            run(args, folder, image, False, transfer(0x46, 0x11, False))
            run(args, folder, image, False, transfer(0x45, 0x55, False), True)
            run(args, folder, image, False,
                transfer(0x46, 0x55, False) + transfer(0x46, 0x77, False))
            run(args, folder, image, False, fast_transfer(0x46, 0x33), selector=0)
            run(args, folder, image, False, fast_transfer(0x45, 0x66), True, selector=0)
            run(args, folder, image, False, fast_transfer(0x46, 0x66), selector=0)
            run(args, folder, image, False,
                transfer(0x46, 0x55, False) + transfer(0x46, 0x77, False))
            assert image.read_bytes() == original
            print(f'{extension}: both MFM rates, selected duplicate and restart passed',
                  flush=True)


if __name__ == '__main__':
    main()
