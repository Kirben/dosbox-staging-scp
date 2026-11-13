#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Measure SPECIFY step, head-load and head-unload timers in guest time."""

import argparse
from pathlib import Path
from struct import unpack
from tempfile import TemporaryDirectory

from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS, SAVE
from run_floppy_id_search_probe import wait_microseconds
from run_floppy_transfer_edges_probe import RESET, program
from run_transcopy_layout_probe import run_guest

POLL = '''
    mov ecx,2000000
poll_completion:
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,80h
    jne seek_complete
    dec ecx
    jnz poll_completion
    jmp done
seek_complete:
    cmp al,20h
    jne done
    in al,dx
    cmp al,2
    jne done
'''
READ_ID = '''
    mov si,id_command
    mov cx,2
    call send_command
    call await_id
'''
ID_ROUTINE = '''
id_command db 4ah,0
await_id:
    mov ecx,4000000
poll_id:
    mov dx,3f4h
    in al,dx
    and al,0d0h
    cmp al,0d0h
    je id_ready
    dec ecx
    jnz poll_id
    jmp done
id_ready:
    mov dx,3f5h
    in al,dx
    test al,0c0h
    jnz done
    mov cx,6
id_result:
    in al,dx
    loop id_result
    ret
'''
SAVE_CLOCK = SAVE.replace('    in al,08h\n    test al,4\n    jz done\n', '')


# Each interval includes the guest command and its PIT timestamps.
# pylint: disable-next=too-many-arguments
def measure(args, directory, image, checks, commands, *, machine):
    """Return the interval between PIT timestamps after a successful command."""
    clock = directory / 'CLOCK.BIN'
    clock.unlink(missing_ok=True)
    body = program(checks + SAVE_CLOCK, commands + DECLARATIONS + ID_ROUTINE)
    if machine == 'pcjr':
        body = body.replace('mov al,1ch', 'mov al,81h').replace('3f2h', '0f2h')
        body = body.replace('3f4h', '0f4h').replace('3f5h', '0f5h')
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              body,
              machine=machine, extra_commands=('config -set cpu cpu_cycles=20000',))
    tick0, pit0, tick1, pit1 = unpack('<IHIH', clock.read_bytes())
    return (65536 * (tick1 - tick0) + pit0 - pit1) * 1000 / 1193182


def check_seek(args, directory, image):
    """Check rate extremes, intermediate rates and PCjr non-DMA selection."""
    for machine in ('svga_s3', 'pcjr'):
        rates = ((2, 250),) if machine == 'pcjr' else ((0, 500), (1, 300),
                                                                  (2, 250), (3, 1000))
        for selector, rate in rates:
            for step in (0, 1, 13, 15):
                checks = RESET + f'''
    mov dx,3f7h
    mov al,{selector}
    out dx,al
    mov si,specify
    mov cx,3
    call send_command
''' + CLOCK_SETUP + '''
    mov di,timestamps
    call stamp
    mov si,seek
    mov cx,3
    call send_command
''' + POLL
                commands = f'specify db 3,{step << 4},{int(machine == "pcjr")}\n'
                elapsed = measure(args, directory, image, checks,
                                  commands + 'seek db 0fh,0,2\n', machine=machine)
                expected = 2 * (16 - step) * 500 / rate
                assert abs(elapsed - expected) < 0.5, (machine, rate, step, elapsed, expected)
                print(f'{machine} {rate} kbit/s SRT={step:x}: seek {elapsed:.3f} ms', flush=True)


def check_heads(args, directory, image):
    """Check first load, retained heads, unload expiry and zero timer codes."""
    for machine in ('svga_s3', 'pcjr'):
        for unload, load, pause, warm in ((1, 16, 0, False), (1, 16, 0, True),
                                         (1, 16, 40000, True), (0, 16, 40000, True),
                                         (1, 0, 0, False)):
            checks = RESET + '''
    mov si,specify
    mov cx,3
    call send_command
'''
            if warm:
                checks += READ_ID + wait_microseconds(pause)
            checks += CLOCK_SETUP + 'mov di,timestamps\ncall stamp\n' + READ_ID
            commands = f'specify db 3,{0xf0 | unload},{(load << 1) | int(machine == "pcjr")}\n'
            elapsed = measure(args, directory, image, checks, commands, machine=machine)
            expected = 4 * (load or 128) if not warm or pause and unload else 0
            assert expected <= elapsed <= expected + 24, (machine, unload, load, pause,
                                                         warm, elapsed, expected)
            print(f'{machine} HUT={unload:x} HLT={load:x} warm={warm} '
                  f'pause={pause}: Read ID {elapsed:.3f} ms', flush=True)


def main():
    """Run timing measurements without modifying the mounted source."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', type=Path, required=True)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        image = directory / 'timers.img'
        original = bytes(360 * 1024)
        image.write_bytes(original)
        check_seek(args, directory, image)
        check_heads(args, directory, image)
        assert image.read_bytes() == original


if __name__ == '__main__':
    main()
