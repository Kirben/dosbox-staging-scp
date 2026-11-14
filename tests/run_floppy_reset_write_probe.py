#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Persist only the selected field's written cells when DOR resets a write."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_fm_probe import fm_byte, fm_image, fm_track
from run_floppy_id_search_probe import scp_turns
from run_floppy_read_deleted_probe import make_track
from run_floppy_transfer_edges_probe import DMA
from run_floppy_write_deleted_probe import routines, setup, transfer
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def reset(pio):
    """Keep the motor running during reset, drain reset status, then rephase it."""
    port = 0xf2 if pio else 0x3f2
    low, high, stopped = (0x01, 0x81, 0x80) if pio else (0x18, 0x1c, 0x0c)
    return f'''
    mov dx,{port}
    mov al,{low}
    out dx,al
    mov al,{high}
    out dx,al
    call drain_reset
    mov dx,{port}
    mov al,{stopped}
    out dx,al
    mov al,{high}
    out dx,al
'''


def wait_crc(fm, pio):
    """Reach CRC emission after the last host byte, using PIT ticks."""
    duration = (160 if pio else 128) if fm else (64 if pio else 48)
    ticks = round(duration * 1.193182)
    return f'''
    pushf
    cli
    xor al,al
    out 43h,al
    in al,40h
    mov ah,al
    in al,40h
    xchg al,ah
    mov bx,ax
crc_delay:
    xor al,al
    out 43h,al
    in al,40h
    mov ah,al
    in al,40h
    xchg al,ah
    mov dx,bx
    sub dx,ax
    cmp dx,{ticks}
    jb crc_delay
    popf
'''


def write_checks(pio, fm, phase):
    """Watch actual host transfers, including FIFO prefetch ahead of the disk."""
    port = 0xf4 if pio else 0x3f4
    code = DMA + 'mov di,buffer\nmov cx,512\nmov al,55h\nrep stosb\n'
    code += 'mov al,4ah\nout 0bh,al\n'
    if pio:
        code += 'mov al,6\nout 0ah,al\n'
    if phase in ('mid', 'history'):
        code += '''
    mov si,configure
    mov cx,4
    call send_command
'''
    code += f'''
    mov byte [command],{0x05 if fm else 0x45}
    mov byte [command+5],2
    mov si,command
    mov cx,9
    call send_command
'''
    if phase == 'before':
        return code + reset(pio)
    if phase == 'after':
        return (code + f'call wait_result\ncmp byte [ids+1],{0x80 if pio else 0}\njne done\n'
                + reset(pio))
    count = 512 if phase == 'crc' else 64
    if pio:
        code += f'''
    mov bp,{count}
    mov ecx,2000000
write_poll:
    mov dx,{port}
    in al,dx
    and al,0f0h
    cmp al,0b0h
    jne write_next
    inc dx
    mov al,55h
    out dx,al
    dec bp
    jz write_reached
write_next:
    dec ecx
    jnz write_poll
    jmp done
write_reached:
'''
    else:
        code += f'''
    mov ecx,2000000
dma_poll:
    xor al,al
    out 0ch,al
    in al,05h
    mov ah,al
    in al,05h
    xchg al,ah
    cmp ax,{511 - count if count < 512 else 0xffff}
    {'jbe' if count < 512 else 'je'} write_reached
    dec ecx
    jnz dma_poll
    jmp done
write_reached:
'''
    if phase == 'crc':
        code += wait_crc(fm, pio)
    return code + reset(pio)


def read_checks(pio, fm, size_code=2):
    """Save result bytes and payload; retain CRC bytes through a long Read Track."""
    size = 128 << size_code
    code = DMA.replace('mov cx,512', f'mov cx,{size + 16}')
    code = code.replace('mov al,1\n    out 05h,al', f'mov al,{(size - 1) >> 8}\n    out 05h,al')
    if pio:
        code += 'mov al,6\nout 0ah,al\n'
    code += f'''
    mov byte [command],{(0x42 if size_code == 3 else 0x46) & (~0x40 if fm else 0xff)}
    mov byte [command+2],0
    mov byte [command+3],0
    mov byte [command+4],1
    mov byte [command+5],{size_code}
    mov byte [command+6],1
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    cmp word [io_count],{size if pio else 0}
    jne done
    mov dx,ids
    mov cx,7
    call save_bytes
    mov dx,buffer
    mov cx,{size}
    call save_bytes
    cmp byte [buffer+{size}],0a5h
    jne done
    inc byte [stage]
'''
    return code


AUXILIARY = '''
configure db 13h,0,0fh,0
snapshot db 'SNAP.BIN',0
handle dw 0
save_bytes:
    mov bx,[handle]
    mov ah,40h
    int 21h
    jc done
    cmp ax,cx
    jne done
    ret
'''


# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def run(args, directory, image, pio, fm, writing, phase):
    """Snapshot both physical duplicates immediately and in a fresh process."""
    code = setup(pio)
    if writing:
        if phase == 'history':
            code += transfer(0x45, 0x33, pio).replace(
                '    call send_command',
                '    mov si,command\n    mov cx,9\n    call send_command')
            code += reset(pio)
        code += write_checks(pio, fm, phase)
    code += '''
    mov dx,snapshot
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc done
    mov [handle],ax
'''
    code += read_checks(pio, fm) * 2 + read_checks(pio, fm, 3)
    code += 'mov bx,[handle]\nmov ah,3eh\nint 21h\n'
    functions = routines(pio).replace('    mov cx,9\n', '')
    functions = functions.replace('    mov si,command\n', '')
    functions = functions.replace('checked:\n', AUXILIARY + 'checked:\n')
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              code + functions, read_only=not writing,
              machine='pcjr' if pio else 'svga_s3',
              extra_commands=('config -set cpu cpu_cycles=20000',))
    return (directory / 'SNAP.BIN').read_bytes()


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check(args, directory, extension, pio, fm, phase, turns=1):
    """An aborted write retains old CRC/tail cells and only changes one duplicate."""
    image = directory / f'reset-{pio}-{fm}-{phase}-{turns}.{extension}'
    cells = (fm_byte(0xff) * 128 + fm_track(2))[:6250] if fm else make_track(
        ((1, 0xfb, '', 0x11), (1, 0xfb, '', 0x77)), 2, lead_gap=200)
    original = fm_image(extension, cells) if fm else encode_tracks(extension, [cells])
    if turns > 1:
        original = scp_turns([cells] * turns)
    image.write_bytes(original)
    snapshots = [run(args, directory, image, pio, fm, writing, phase)
                 for writing in (True, False)]
    assert snapshots[0] == snapshots[1], (image.name, 'restart mismatch')
    snapshot = snapshots[0]
    first, second = snapshot[:519], snapshot[519:1038]
    raw = snapshot[1045:]
    assert len(raw) == 1024 and second[7:] == bytes([0x22 if fm else 0x77]) * 512
    assert second[1] == (0x80 if pio else 0) and second[2] == 0, second[:7].hex()
    overlay = Path(str(image) + '.overlay')
    old = 0x33 if phase == 'history' else 0x11
    if phase == 'before':
        assert first[7:] == bytes([old]) * 512 and first[2] == 0
        assert not overlay.exists()
    elif phase == 'after':
        assert first[7:] == b'\x55' * 512 and first[2] == 0
    else:
        saved = overlay.read_bytes()
        assert saved[6] == 12
        if turns > 1:
            assert len(saved) == 16 + turns * 525
            slots = [int.from_bytes(saved[19 + i * 525:23 + i * 525], 'little')
                     for i in range(turns)]
            assert slots == list(range(1, turns * 2, 2)), slots
        # A normal predecessor in the history case occupies 9+512 bytes.
        offset = 16 + (521 if phase == 'history' else 0)
        assert saved[offset + 1] == 0xfd
        assert int.from_bytes(saved[offset + 3:offset + 7], 'little') == 1
        flags = saved[offset + 8]
        assert flags == (6 if fm else 4)
        bits = int.from_bytes(saved[offset + 9:offset + 13], 'little')
        payload_bits = max(0, min(8192, bits - 16))
        complete, rest = divmod(payload_bits // 2, 8)
        expected = bytearray([old] * 512)
        expected[:complete] = b'\x55' * complete
        if rest:
            mask = (0xff << (8 - rest)) & 255
            expected[complete] = (0x55 & mask) | (old & ~mask)
        assert first[7:] == expected, (image.name, bits, first[7:80].hex())
        assert first[1:3] == b'\x20\x20', (image.name, first[:7].hex())
        verify_crc(raw, fm, old, bits, phase)
    assert image.read_bytes() == original
    if not pio and not fm and phase == 'mid':
        check_invalid(args, directory, image, saved)
    print(f'{extension} {"FM" if fm else "MFM"} {"PIO" if pio else "DMA"} '
          f'{phase}: selected duplicate, CRC, old suffix, recovery and restart passed', flush=True)


def verify_crc(raw, fm, old, bits, phase):
    """Long Read Track must expose the original or partly rewritten CRC cells."""
    seed = b'' if fm else b'\xa1' * 3
    old_crc = crc_hqx(seed + b'\xfb' + bytes([old]) * 512, 0xffff)
    if phase == 'crc':
        assert 8208 < bits < 8240, bits
        new_crc = crc_hqx(seed + b'\xfb' + b'\x55' * 512, 0xffff)
        mask = (0xffff << (16 - (bits - 8208) // 2)) & 0xffff
        expected = (new_crc & mask) | (old_crc & ~mask)
    else:
        assert 16 + 32 * 16 < bits < 16 + 96 * 16, bits
        expected = old_crc
    assert raw[512:514] == expected.to_bytes(2, 'big'), raw[512:514].hex()


def check_invalid(args, directory, image, saved):
    """Reject invalid counts, flags and truncated partial-write records."""
    overlay = Path(str(image) + '.overlay')
    for count in (0, 8241):
        invalid = bytearray(saved)
        invalid[25:29] = count.to_bytes(4, 'little')
        overlay.write_bytes(invalid)
        run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                  'mov ax,0800h\nxor dx,dx\nint 13h\njnc done\n')
        assert overlay.read_bytes() == invalid
    invalid = bytearray(saved)
    invalid[24] = 8
    for malformed in (invalid, saved[:27], saved[:-1]):
        overlay.write_bytes(malformed)
        run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                  'mov ax,0800h\nxor dx,dx\nint 13h\njnc done\n')
        assert overlay.read_bytes() == malformed
    overlay.write_bytes(saved)


def check_raw(args, directory, extension):
    """Raw files retain the data prefix, without inventing CRC metadata."""
    image = directory / f'raw-reset.{extension}'
    original = b'\x11' * (360 * 1024)
    image.write_bytes(original)
    snapshots = []
    functions = routines(False).replace('    mov cx,9\n', '')
    functions = functions.replace('    mov si,command\n', '')
    functions = functions.replace('checked:\n', AUXILIARY + 'checked:\n')
    for writing in (True, False):
        code = setup(False) + (write_checks(False, False, 'mid') if writing else '')
        code += '''
    mov dx,snapshot
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc done
    mov [handle],ax
''' + read_checks(False, False) + 'mov bx,[handle]\nmov ah,3eh\nint 21h\n'
        run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                  code + functions, read_only=not writing,
                  extra_commands=('config -set cpu cpu_cycles=20000',))
        snapshots.append((directory / 'SNAP.BIN').read_bytes())
    assert snapshots[0] == snapshots[1]
    assert snapshots[0][1:3] == b'\0\0'
    data = snapshots[0][7:]
    prefix = len(data) - len(data.lstrip(b'\x55'))
    assert 32 <= prefix < 96 and data[prefix + 1:] == b'\x11' * (511 - prefix)
    assert image.read_bytes() == data + original[512:]
    print(f'{extension}: partial prefix survives reset/restart; sector-only CRC control passed',
          flush=True)


def main():
    """Exercise reset before data, in payload/CRC, and after completion."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    parser.add_argument('--formats', nargs='+', default=['scp', 'mfm', '86f', 'tc'])
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        for extension in args.formats:
            for pio in (False, True):
                for fm in (False, True):
                    phases = ('mid', 'crc') if fm else ('before', 'mid', 'crc', 'after', 'history')
                    for phase in phases:
                        check(args, Path(temporary), extension, pio, fm, phase)
        for extension in ('img', 'ima'):
            check_raw(args, Path(temporary), extension)
        for pio in (False, True):
            check(args, Path(temporary), 'scp', pio, False, 'mid', turns=5)


if __name__ == '__main__':
    main()
