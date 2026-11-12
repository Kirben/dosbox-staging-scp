#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check normal Read Track completion by EOT without DMA terminal count."""
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, DMA, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


# pylint: disable-next=too-many-locals
def main():
    """Run the guest regression against the selected emulator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ('tc', 'mfm', 'scp', '86f'):
            for opcode, bad_crc, length in ((0x42, False, 512), (0x42, False, 1024),
                                            (0x46, False, 1024)):
                image = directory / f'crc{bad_crc}.{extension}'
                original = encode_tracks(extension, [one_sector(2, bad_crc)])
                image.write_bytes(original)
                st0 = 0x40 if opcode == 0x46 else 0
                st1 = 0x80 if opcode == 0x46 else 0
                st2 = 0x20 if bad_crc else 0
                dma = DMA.replace('mov cx,512', 'mov cx,1040').replace(
                    'mov al,1\n    out 05h,al', f'mov al,{(length - 1) >> 8}\n    out 05h,al')
                checks = RESET + dma + f'''
        mov si,command
        mov cx,9
        call send_command
        call wait_result
        cmp byte [ids],{st0}
        jne done
        cmp byte [ids+1],{st1}
        jne done
        cmp byte [ids+2],{st2}
        jne done
        in al,08h
        test al,4
        {'jz' if length == 512 else 'jnz'} done
        mov si,buffer
        mov cx,512
    payload:
        lodsb
        cmp al,77h
        jne done
        loop payload
        mov cx,16
    guard:
        lodsb
        cmp al,0a5h
        jne done
        loop guard
    '''
                commands = f'command db {opcode},0,0,0,1,2,1,54h,0ffh\n'
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          program(checks, commands))
                assert image.read_bytes() == original
                print(f'.{extension} {"Read Track" if opcode == 0x42 else "Read Data"}, '
                      f'bad CRC={bad_crc}, DMA length={length}: '
                      f'ST0={st0:02x}/ST1={st1:02x}/ST2={st2:02x}; '
                      f'TC={length == 512}, payload, guard and IRQ6 verified', flush=True)


if __name__ == "__main__":
    main()
