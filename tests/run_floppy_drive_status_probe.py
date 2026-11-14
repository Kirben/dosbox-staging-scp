#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check 82077AA ST3, physical TRACK0 and non-interrupting status commands."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_floppy_transfer_edges_probe import one_sector
from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import launch


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check_status(dosbox, nasm, directory, first, second, unit, machine):
    """Exercise status before/after media changes and pending Seek completion."""
    source = Path(__file__).with_name('floppy_drive_status_probe.asm')
    subprocess.run([nasm, '-f', 'bin', f'-DUNIT={unit}', f'-DPCJR={int(machine == "pcjr")}',
                    source, '-o', directory / 'PROBE.COM'], check=True)
    for phase in range(5):
        (directory / f'STATUS{phase}.OUT').unlink(missing_ok=True)
    fixed = directory / 'fixed.img'
    fixed.write_bytes(bytes(16 * 4 * 17 * 512))
    commands = [f'mount c "{directory}"', 'c:',
                f'mount 2 "{fixed}" -t hdd -fs none -size 512,17,4,16 -ro',
                'config -set cpu cpu_cycles=20000', 'probe 0',
                f'mount {unit} "{first}" -t floppy -fs none',
                f'mount {unit ^ 1} "{second}" -t floppy -fs none -ro', 'probe 1',
                f'mount {unit} "{first}" -t floppy -fs none -ro', 'probe 2',
                f'mount -u {"ab"[unit]}', 'probe 3',
                f'mount {unit} "{first}" -t floppy -fs none', 'probe 4', 'exit']
    command, environment = launch(dosbox, directory, 'status', commands)
    if machine != 'pc':
        config = command[-1]
        config.write_text(f'[dosbox]\nmachine={machine}\n' + config.read_text(),
                          encoding='ascii')
    run = subprocess.run(command, cwd=directory, env=environment, timeout=30,
                         capture_output=True, text=True, check=True)
    for phase in range(5):
        report = directory / f'STATUS{phase}.OUT'
        result = report.read_bytes() if report.exists() else b''
        if result != b'\0':
            raise RuntimeError(f'{first.suffix} {machine} drive {"AB"[unit]}, '
                               f'phase {phase}, stage {result.hex()}\n'
                               f'{run.stdout}{run.stderr}')
    print(f'{first.suffix} {machine} {"AB"[unit]}: ST3, physical TRACK0, WP swaps, '
          'ejection, motors, result framing and pending Seek IRQ/status passed', flush=True)


def main():
    """Run synthetic guest checks across raw and all writable track backends."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    parser.add_argument('--formats', nargs='+', default=['img', 'ima', 'scp', 'mfm', '86f', 'tc'])
    parser.add_argument('--machines', nargs='+', default=['pc', 'pcjr', 'tandy'])
    parser.add_argument('--units', nargs='+', type=int, choices=(0, 1), default=[0, 1])
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in args.formats:
            original = bytes(360 * 1024) if extension in ('img', 'ima') else encode_tracks(
                extension, [one_sector(2)] * 3)
            first, second = (directory / f'{name}.{extension}' for name in ('first', 'second'))
            first.write_bytes(original)
            second.write_bytes(original)
            for machine in args.machines:
                for unit in args.units:
                    check_status(args.dosbox.resolve(), args.nasm, directory,
                                 first, second, unit, machine)
                    assert first.read_bytes() == second.read_bytes() == original


if __name__ == '__main__':
    main()
