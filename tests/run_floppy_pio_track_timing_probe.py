#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare first PIO byte timing for Read Data and Read Track."""
from pathlib import Path
from struct import unpack
from tempfile import TemporaryDirectory
import argparse

from run_floppy_transfer_edges_probe import RESET, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS
from run_floppy_id_search_probe import wait_microseconds
from run_transcopy_layout_probe import run_guest
from run_floppy_pio_underrun_timing_probe import SAVE
from run_floppy_read_track_size_probe import ROUTINES as FAST_ROUTINES


def check_complete(args, directory, extension, bad_crc, machine='svga_s3'):
    """Consume a complete PIO field and wait through its recorded CRC."""
    image = directory / f'complete{machine}{bad_crc}.{extension}'
    original = encode_tracks(extension, [one_sector(2, bad_crc)])
    image.write_bytes(original)
    checks = (RESET if machine != 'pcjr' else '''
    mov dx,3f2h
    xor al,al
    out dx,al
    mov al,81h
    out dx,al
    call drain_reset
''') + CLOCK_SETUP + f'''
    mov si,specify
    mov cx,3
    call send_command
    mov dx,3f2h
    mov al,{0x80 if machine == 'pcjr' else 0x0c}
    out dx,al
    mov di,timestamps
    call stamp
    mov dx,3f2h
    mov al,{0x81 if machine == 'pcjr' else 0x1c}
    out dx,al
    mov si,command
    mov cx,9
    call send_command
    mov bp,512
    mov ecx,2000000
poll_data:
    mov dx,3f4h
    in al,dx
    and al,0f0h
    cmp al,0d0h
    je done
    cmp al,0f0h
    je data_ready
    dec ecx
    jnz poll_data
    jmp done
data_ready:
    mov dx,3f5h
    in al,dx
    cmp al,77h
    jne done
    dec bp
    jnz poll_data
    call wait_result
''' + f'''
    mov al,[ids+1]
    and al,20h
    cmp al,{0x20 if bad_crc else 0}
    jne done
    mov al,[ids+2]
    and al,20h
    cmp al,{0x20 if bad_crc else 0}
    jne done
''' + SAVE
    body = (checks + '\n    jmp checked\n' + DECLARATIONS +
            '\nspecify db 3,0dfh,1\ncommand db 42h,0,0,0,1,2,1,54h,0ffh\n' +
            FAST_ROUTINES + 'checked:\n')
    if machine == 'pcjr':
        for port in ('2', '4', '5'):
            body = body.replace(f'3f{port}h', f'0f{port}h')
    run_guest(args.dosbox.resolve(), args.nasm, directory, image, body,
              extra_commands=('config -set cpu cpu_cycles=20000',), machine=machine)
    t0, p0, t1, p1 = unpack('<IHIH', (directory / 'CLOCK.BIN').read_bytes())
    elapsed = (65536 * (t1 - t0) + p0 - p1) * 1000 / 1193182
    assert 220.928 <= elapsed < 221.1, (extension, bad_crc, elapsed)
    assert image.read_bytes() == original
    print(f'.{extension} {machine} PIO Read Track CRC={bad_crc}: '
          f'full payload and completion at {elapsed:.3f} ms passed', flush=True)


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
            image = directory / f'pio.{extension}'
            original = encode_tracks(extension, [one_sector(2)])
            image.write_bytes(original)
            times = []
            for opcode in (0x46, 0x42):
                checks = RESET + CLOCK_SETUP + '''
        mov si,specify
        mov cx,3
        call send_command
        mov dx,3f2h
        mov al,0ch
        out dx,al
        mov di,timestamps
        call stamp
        mov dx,3f2h
        mov al,1ch
        out dx,al
    ''' + wait_microseconds(190000) + '''
        mov si,command
        mov cx,9
        call send_command
        mov ecx,2000000
    poll_byte:
        mov dx,3f4h
        in al,dx
        and al,0f0h
        cmp al,0f0h
        je ready
        dec ecx
        jnz poll_byte
        jmp done
    ready:
        mov dx,3f5h
        in al,dx
        cmp al,77h
        jne done
        mov di,timestamps+6
        call stamp
        mov dx,3f2h
        xor al,al
        out dx,al
        mov dx,filename
        xor cx,cx
        mov ax,3c00h
        int 21h
        jc done
        mov bx,ax
        mov dx,timestamps
        mov cx,12
        mov ah,40h
        int 21h
        mov ah,3eh
        int 21h
    '''
                commands = DECLARATIONS + ('\nspecify db 3,0dfh,1\n'
                                           f'command db {opcode},0,0,0,1,2,1,54h,0ffh\n')
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          program(checks, commands),
                          extra_commands=('config -set cpu cpu_cycles=20000',))
                t0,p0,t1,p1 = unpack('<IHIH', (directory / 'CLOCK.BIN').read_bytes())
                elapsed = (65536*(t1-t0)+p0-p1)*1000/1193182
                times.append(elapsed)
                assert image.read_bytes() == original
            difference = times[0] - times[1]
            assert abs(difference) < 0.1, (extension, times)
            assert all(204.480 <= value < 204.6 for value in times), (extension, times)
            print(f'.{extension}: first byte Read Data={times[0]:.3f} ms, '
                  f'Read Track={times[1]:.3f} ms; difference {difference:.3f} ms; '
                  'both payload bytes 77h, source unchanged', flush=True)
            for machine in ('svga_s3', 'pcjr', 'tandy'):
                for bad_crc in (False, True):
                    check_complete(args, directory, extension, bad_crc, machine)


if __name__ == "__main__":
    main()
