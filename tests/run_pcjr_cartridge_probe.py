#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep cartridge headers independent of floppy geometry validation."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import launch


def main():
    """List a synthetic ROM command through host and DOS paths; reject bad disks."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        folder = Path(temporary)
        cartridge = bytearray(512 + 8192)
        cartridge[:4] = b'PCjr'
        cartridge[518:526] = b'\x03RUN\x00\x00\x00\x00'
        image = folder / 'CART.JRC'
        image.write_bytes(cartridge)
        (folder / 'BAD.IMG').write_bytes(bytes(512))
        (folder / 'SHORT.JRC').write_bytes(b'PCjr')
        for machine in ('pcjr', 'svga_s3'):
            command, environment = launch(args.dosbox.resolve(), folder, machine, [
                f'mount c "{folder}"', 'c:',
                f'boot "{image}" -e ? > HOST.TXT',
                'boot CART.JRC -e ? > DOS.TXT',
                'boot BAD.IMG > BAD.TXT', 'boot SHORT.JRC > SHORT.TXT', 'exit',
            ])
            config = command[-1]
            config.write_text(f'[dosbox]\nmachine={machine}\n' + config.read_text(),
                              encoding='ascii')
            subprocess.run(command, cwd=folder, env=environment, timeout=30,
                           check=True, capture_output=True)
            expected = ('Available PCjr cartridge commands:' if machine == 'pcjr'
                        else 'PCjr cartridge found, but machine is not PCjr.')
            for name in ('HOST.TXT', 'DOS.TXT'):
                assert expected in (folder / name).read_text(), (machine, name)
                if machine == 'pcjr':
                    assert 'RUN' in (folder / name).read_text(), name
            for name in ('BAD.TXT', 'SHORT.TXT'):
                assert 'Unable to boot' in (folder / name).read_text(), (machine, name)
            print(f'{machine}: cartridge commands and invalid disk controls passed', flush=True)
        assert image.read_bytes() == cartridge


if __name__ == '__main__':
    main()
