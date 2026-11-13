#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check renamed track containers through MOUNT and both BOOT file paths."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_hxc_mfm_probe import check_boot, encode
from run_scp_attachment_probe import encode_scp
from run_transcopy_attachment_probe import encode_transcopy


def main():
    """Boot containers whose suffixes cannot identify their real format."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        for name, encoder in (('mfm', encode), ('scp', encode_scp),
                              ('86f', encode_86f), ('tc', encode_transcopy)):
            directory = root / name
            directory.mkdir()
            # An arbitrary suffix must use content probing, including DOS paths.
            check_boot(args.dosbox.resolve(), args.nasm, directory, encoder, 'bin')
            print(f'{name} renamed .bin: content detection, DMA boot, BIOS R/W/Verify passed',
                  flush=True)


if __name__ == '__main__':
    main()
