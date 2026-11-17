#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check short TC weak intervals, fixed CRC controls and overlay replacement."""

import argparse
from binascii import crc_hqx
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import MFM, encode_track, launch
from run_long_read_track_probe import make_image


INTERVALS = ((64, 3), (128, 4), (193, 4), (256, 5),
             (321, 5), (384, 6), (449, 6), (512, 16))


def field_payload():
    """Place transitions at each test interval's boundaries."""
    payload = bytearray(512)
    for start, length in INTERVALS:
        for cell in (start - 1, start + length):
            if cell & 1:
                payload[cell // 16] |= 0x80 >> ((cell % 16) // 2)
    return payload


def fixture(weak):
    """Remove only clock cells; the fixed control has identical data and CRC."""
    payload = field_payload()
    disk = bytearray(360 * 1024)
    disk[:512] = payload
    cells = encode_track(disk, 0)
    marker = cells.index(b'\x44\x89' * 3 + MFM[1][0xfb])
    data_start = (marker + 8) * 8
    crc = crc_hqx(b'\xa1' * 3 + b'\xfb' + payload, 0xffff) ^ 1
    crc_start = data_start // 8 + 1024
    cells[crc_start:crc_start + 6] = (MFM[payload[-1] & 1][crc >> 8]
                                     + MFM[(crc >> 8) & 1][crc & 255]
                                     + MFM[crc & 1][0x4e])
    mask = bytearray(512)
    if weak:
        for start, length in INTERVALS:
            for cell in range(start, start + length):
                if not cell & 1:
                    absolute = data_start + cell
                    cells[absolute // 8] &= ~(0x80 >> (absolute % 8))
                elif cell >= start + 3:
                    mask[cell // 16] |= 0x80 >> ((cell % 16) // 2)
    return make_image(cells, 'tc'), payload, mask, crc


def check_result(result, sample, crc, command):
    """Check the CRC flag and the existing end-of-track CHRN convention."""
    bad_crc = crc_hqx(b'\xa1' * 3 + b'\xfb' + sample, 0xffff) != crc
    cylinder = int(command == 0x46 and not bad_crc)
    assert result[0] & 7 == 0 and result[3:] == bytes((cylinder, 0, 1, 2)), result.hex()
    assert result[1] & 0x7f == (0x20 if bad_crc else 0), result.hex()
    assert result[2] == (0x20 if bad_crc else 0), result.hex()


def check_samples(folder, payload, mask, crc, command):
    """Require exactly the ambiguous data bits to vary, with correct CRC status."""
    assert (folder / 'RESULT.BIN').read_bytes() == b'\0'
    data = (folder / 'DATA.BIN').read_bytes()
    statuses = (folder / 'STATUS.BIN').read_bytes()
    assert len(data) == 32 * 528 and len(statuses) == 32 * 7
    observed = bytearray(512)
    for turn in range(32):
        sample = data[turn * 528:turn * 528 + 512]
        assert data[turn * 528 + 512:(turn + 1) * 528] == b'\xcc' * 16
        for i, (actual, original) in enumerate(zip(sample, payload)):
            difference = actual ^ original
            assert not difference & ~mask[i], (turn, i, difference, mask[i])
            observed[i] |= difference
        check_result(statuses[turn * 7:(turn + 1) * 7], sample, crc, command)
    assert observed == mask, (observed.hex(), mask.hex())


def run(args, folder, image, operation, expected):
    """Use DMA channel 2 to capture 32 physical reads in a fresh process."""
    command, write = operation
    assembler = [args.nasm, '-f', 'bin', f'-DREAD_COMMAND={command}']
    if write:
        assembler.append('-DWRITE')
    assembler += [Path(__file__).with_name('transcopy_weak_spacing_probe.asm'),
                  '-o', folder / 'PROBE.COM']
    subprocess.run(assembler, check=True)
    for name in ('RESULT.BIN', 'DATA.BIN', 'STATUS.BIN'):
        (folder / name).unlink(missing_ok=True)
    access = '' if write else ' -ro'
    commands, environment = launch(args.dosbox.resolve(), folder, 'weak-spacing', [
        f'mount c "{folder}"', f'mount 0 "{image}" -t floppy -fs none{access}',
        'config -set cpu core=normal', 'config -set cpu cpu_cycles=3000',
        'c:', 'PROBE.COM', 'exit',
    ])
    subprocess.run(commands, cwd=folder, env=environment, capture_output=True,
                   timeout=30, check=True)
    check_samples(folder, *expected, command)


def main():
    """Keep fixed bad-CRC data stable; writes replace weak cells persistently."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', type=Path, required=True)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory(prefix='tc-weak-spacing-') as temporary:
        folder = Path(temporary)
        for weak in (False, True):
            original, payload, mask, crc = fixture(weak)
            image = folder / f'weak-{weak}.tc'
            image.write_bytes(original)
            for command in (0x46, 0x42):
                run(args, folder, image, (command, False), (payload, mask, crc))
                assert image.read_bytes() == original
                assert not Path(str(image) + '.overlay').exists()
            print(f'TC weak={weak}: exact varying mask, CRC status and guard passed', flush=True)
        payload = bytes([0x66]) * 512
        crc = crc_hqx(b'\xa1' * 3 + b'\xfb' + payload, 0xffff)
        expected = payload, bytes(512), crc
        run(args, folder, image, (0x46, True), expected)
        overlay = Path(str(image) + '.overlay')
        saved = overlay.read_bytes()
        run(args, folder, image, (0x42, False), expected)
        assert overlay.read_bytes() == saved and image.read_bytes() == original
        print('TC short weak cells: write replacement and restart persistence passed', flush=True)


if __name__ == '__main__':
    main()
