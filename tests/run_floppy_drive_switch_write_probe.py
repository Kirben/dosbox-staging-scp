#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Retain the selected duplicate's partial write when DOR selects another drive."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_reset_write_probe import check


def main():
    """Check old suffixes, CRCs, recovery and persistent duplicate contents."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    args.motor_loss = True
    args.drive_switch = True
    args.unit = 0
    with TemporaryDirectory() as temporary:
        for extension in ('scp', 'mfm', '86f', 'tc'):
            for fm in (False, True):
                for phase in ('before', 'mid', 'crc', 'after'):
                    check(args, Path(temporary), extension, False, fm, phase)


if __name__ == '__main__':
    main()
