#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check the next CHRN reported after normal DMA terminal count."""
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, DMA, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def main():
    """Run the regression with an explicitly selected emulator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ('img', 'ima', 'tc', 'mfm', 'scp', '86f'):
            for writing in (False, True):
                for eot in (2, 1):
                    image = directory / f'tc{writing}{eot}.{extension}'
                    original = bytearray(360 * 1024) if extension in ('img', 'ima') else None
                    if original is not None:
                        original[:512] = b'\x77' * 512
                    else:
                        original = encode_tracks(extension, [one_sector(2)])
                    image.write_bytes(original)
                    checks = RESET + DMA
                    if writing:
                        checks += '''
        mov di,buffer
        mov cx,512
        mov al,77h
        rep stosb
        mov al,4ah
        out 0bh,al
    '''
                    checks += f'''
        mov si,command
        mov cx,9
        call send_command
        call wait_result
        cmp byte [ids],0
        jne done
        cmp byte [ids+1],0
        jne done
        cmp byte [ids+2],0
        jne done
        in al,08h
        test al,4
        jz done
        cmp byte [buffer],77h
        jne done
        cmp word [ids+3],{0 if eot == 2 else 1}
        jne done
        cmp word [ids+5],{0x0202 if eot == 2 else 0x0201}
        jne done
    '''
                    command = f'command db {0x45 if writing else 0x46},0,0,0,1,2,{eot},54h,0ffh\n'
                    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                              program(checks, command),
                              read_only=not writing)
                    assert image.read_bytes() == original
                    expected = 'C0/H0/R2/N2' if eot == 2 else 'C1/H0/R1/N2'
                    print(f'{extension} {"Write" if writing else "Read"} Data EOT={eot}: '
                          f'data, DMA TC and next position {expected} passed',
                          flush=True)


if __name__ == "__main__":
    main()
