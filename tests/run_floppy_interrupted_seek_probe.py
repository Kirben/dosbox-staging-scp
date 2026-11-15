#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify reset preserves completed seek steps on all floppy backends."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import encode_track
from run_floppy_id_search_probe import wait_microseconds
from run_floppy_transfer_edges_probe import RESET, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def main():
    """Compare resets before, during and after a ten-step seek."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', type=Path, required=True)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory(prefix='interrupted-seek-audit-') as temporary:
        folder = Path(temporary)
        disk = bytes(360 * 1024)
        tracks = [encode_track(disk, cylinder * 2) for cylinder in range(11)]
        for extension in ('img', 'ima', 'scp', 'mfm', '86f', 'tc'):
            original = disk if extension in ('img', 'ima') else encode_tracks(extension, tracks)
            image = folder / f'seek.{extension}'
            image.write_bytes(original)
            for duration in (0, 30000, 70000):
                body = RESET + wait_microseconds(500000) + '''
        mov si,specify
        mov cx,3
        call send_command
        mov si,seek
        mov cx,3
        call send_command
    '''
                if duration:
                    body += wait_microseconds(duration)
                body += '''
        mov dx,3f2h
        mov al,18h
        out dx,al
        mov al,1ch
        out dx,al
        call drain_reset
        call read_id
        mov si,ids
        mov di,saved
        mov cx,7
        rep movsb
        mov dx,3f5h
        mov al,4
        out dx,al
        xor al,al
        out dx,al
        in al,dx
        mov [saved+7],al
        mov dx,output
        xor cx,cx
        mov ah,3ch
        int 21h
        mov bx,ax
        mov dx,saved
        mov cx,8
        mov ah,40h
        int 21h
        mov ah,3eh
        int 21h
    '''
                run_guest(args.dosbox.resolve(), args.nasm, folder, image, program(body,
                          "specify db 3,0dfh,2\nseek db 0fh,0,10\n"
                          "saved times 8 db 0\noutput db 'SNAP.BIN',0\n"))
                data = (folder / 'SNAP.BIN').read_bytes()
                assert data[:3] == bytes(3), (extension, duration, data.hex())
                expected = {0} if duration == 0 else {4, 5} if duration == 30000 else {10}
                assert data[3] in expected, (extension, duration, data.hex())
                assert bool(data[7] & 0x10) == (duration == 0), (extension, duration, data.hex())
                print(f'{extension} reset after {duration} us (SRT=6 ms): '
                      f'physical C={data[3]} ST3={data[7]:02x}', flush=True)
            assert image.read_bytes() == original


if __name__ == '__main__':
    main()
