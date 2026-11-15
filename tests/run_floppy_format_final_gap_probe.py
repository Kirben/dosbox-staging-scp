#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that interruption in final format gap preserves the unwritten suffix."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_contract_probe import Case, guest
from run_floppy_read_deleted_probe import make_track
from run_floppy_id_search_probe import wait_microseconds
from run_floppy_transfer_edges_probe import RESET, DMA, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


# pylint: disable-next=too-many-locals
def main():
    """Compare reset and motor loss with complete and early-TC controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', type=Path, required=True)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory(prefix='final-format-gap-audit-') as temporary:
        folder = Path(temporary)
        cells = (make_track(((1, 0xfb, '', 0x11),), 2)[:6250] +
                 make_track(((2, 0xfb, '', 0x22),), 2)[:6250])
        for extension in ('scp', 'mfm', '86f', 'tc'):
            for mode in ('normal', 'reset', 'motor', 'late-reset', 'early-tc-control',
                         'reset-after-write'):
                image = folder / f'gap-{mode}.{extension}'
                original = encode_tracks(extension, [cells])
                image.write_bytes(original)
                tail = 0xa6 if mode == 'reset-after-write' else 0x22
                if mode == 'reset-after-write':
                    write = guest([Case('prior-tail-write', opcode=0x45, sector=2,
                                        marker=tail, transferred=512)])
                    run_guest(args.dosbox.resolve(), args.nasm, folder, image,
                              write, read_only=False)
                body = RESET + DMA.replace('mov al,0ffh\n    out 05h,al',
                                           'mov al,3\n    out 05h,al').replace(
                                               'mov al,1\n    out 05h,al',
                                               'xor al,al\n    out 05h,al')
                body += '''
        mov dword [buffer],02090000h
        mov al,4ah
        out 0bh,al
        mov si,command
        mov cx,6
        call send_command
    '''
                delay = 500000 if mode == 'normal' else 350000 if mode == 'late-reset' else 250000
                body += wait_microseconds(delay)
                body += 'mov dx,3f4h\nin al,dx\nmov [saved],al\n'
                if mode in ('reset', 'late-reset', 'reset-after-write'):
                    body += 'mov dx,3f2h\nmov al,18h\nout dx,al\nmov al,1ch\nout dx,al\n'
                if mode == 'motor':
                    body += 'mov dx,3f2h\nmov al,0ch\nout dx,al\n'
                body += '''
        mov dx,output
        xor cx,cx
        mov ah,3ch
        int 21h
        mov bx,ax
        mov dx,saved
        mov cx,1
        mov ah,40h
        int 21h
        mov ah,3eh
        int 21h
    '''
                count = 2 if mode == 'early-tc-control' else 1
                run_guest(args.dosbox.resolve(), args.nasm, folder, image, program(body,
                          f"command db 4dh,0,2,{count},54h,0e5h\n"
                          "saved db 0\noutput db 'PHASE.BIN',0\n"),
                          read_only=False)
                phase = (folder / 'PHASE.BIN').read_bytes()[0]
                replay = RESET
                for index in range(4):
                    replay += f'call read_id\nmov al,[ids+5]\nmov [saved+{index}],al\n'
                replay += DMA + '''
        mov si,command
        mov cx,9
        call send_command
        call wait_result
        mov al,[ids+1]
        mov [saved+4],al
        mov al,[buffer]
        mov [saved+5],al
        mov dx,output
        xor cx,cx
        mov ah,3ch
        int 21h
        mov bx,ax
        mov dx,saved
        mov cx,6
        mov ah,40h
        int 21h
        mov ah,3eh
        int 21h
    '''
                if mode not in ('normal', 'late-reset'):
                    track = DMA.replace('mov al,1\n    out 05h,al',
                                        'mov al,3\n    out 05h,al') + """
    mov si,track_command
    mov cx,9
    call send_command
    call wait_result
    mov al,[buffer]
    mov [saved+6],al
    mov al,[buffer+512]
    mov [saved+7],al
    mov al,[ids+1]
    mov [saved+8],al
"""
                    replay = replay.replace('        mov dx,output',
                                            track + '        mov dx,output')
                replay = replay.replace('mov cx,6', 'mov cx,9')
                run_guest(args.dosbox.resolve(), args.nasm, folder, image, program(replay,
                          "command db 46h,0,0,0,2,2,2,54h,0ffh\n"
                          "track_command db 42h,0,0,0,1,2,2,54h,0ffh\n"
                          "saved times 9 db 0\noutput db 'SNAP.BIN',0\n"))
                data = (folder / 'SNAP.BIN').read_bytes()
                if mode in ('normal', 'late-reset'):
                    assert set(data[:4]) == {9} and data[4:6] == bytes((4, 0xa5)), data.hex()
                else:
                    assert set(data[:4]) == {9, 2} and data[4:6] == bytes((0, tail)), data.hex()
                    assert phase & 0x10, (mode, phase)
                    assert data[6:] == bytes((0xe5, tail, 0)), (mode, data.hex())
                print(f'{extension} {mode}: MSR before interruption={phase:02x}, '
                      f'restart IDs={list(data[:4])}, R2 data={data[5]:02x}', flush=True)
                assert image.read_bytes() == original


if __name__ == '__main__':
    main()
