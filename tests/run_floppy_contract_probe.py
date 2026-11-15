#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply one FDC hardware contract to raw and captured-track backends."""

import argparse
from dataclasses import dataclass
from pathlib import Path
from struct import Struct
from tempfile import TemporaryDirectory

from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS
from run_floppy_id_search_probe import scp_turns
from run_floppy_read_deleted_probe import make_track
from run_floppy_transfer_edges_probe import DMA, RESET, ROUTINES
from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import encode_track
from run_transcopy_layout_probe import run_guest


RECORD = Struct('<7sHBBIHIH528s')
FORMATS = ('img', 'ima', 'scp', 'mfm', '86f', 'tc')


@dataclass(frozen=True)
# The command and independent expected observations form one contract case.
# pylint: disable-next=too-many-instance-attributes
class Case:
    """One command with a fixed 512-byte DMA allocation and its expected result."""
    name: str
    opcode: int = 0x46
    sector: int = 1
    cylinder: int = 0
    rate: int = 2
    marker: int = 0xa5
    status: tuple = (0, 0, 0)
    transferred: int = 0
    position: int | None = None
    eot: int = 255


def source_disk():
    """Give every C0 sector a distinct marker and protect another cylinder."""
    disk = bytearray(b'\x70' * (360 * 1024))
    for sector in range(1, 10):
        disk[(sector - 1) * 512:sector * 512] = bytes([0x10 + sector]) * 512
    disk[19 * 512:20 * 512] = b'\x62' * 512
    return disk


SEEK = '''
    mov si,seek_command
    mov cx,3
    call send_command
    mov ah,86h
    xor cx,cx
    mov dx,20000
    int 15h
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,20h
    jne done
    in al,dx
    cmp al,[seek_command+2]
    jne done
'''


OBSERVE = '''
observe:
    mov si,ids
    mov di,record
    mov cx,7
    rep movsb
    xor al,al
    out 0ch,al
    in al,05h
    mov [record+7],al
    in al,05h
    mov [record+8],al
    in al,08h
    and al,4
    mov [record+9],al
    push es
    push word 40h
    pop es
    mov al,[es:3eh]
    test al,80h
    pop es
    jz done
    mov al,[irq_count]
    mov [record+10],al
    mov di,record+17
    call stamp
    mov si,buffer
    mov di,record+23
    mov cx,528
    rep movsb
    mov bx,[handle]
    mov dx,record
    mov cx,551
    mov ah,40h
    int 21h
    jc done
    cmp ax,551
    jne done
    ret
handle dw 0
filename db 'CONTRACT.BIN',0
record times 551 db 0
seek_command db 0fh,0,1
old_irq6 dd 0
irq_count db 0
count_irq6:
    inc byte [cs:irq_count]
    jmp far [cs:old_irq6]
'''


def guest(cases):
    """Capture observations; Python supplies expectations independently."""
    code = RESET + CLOCK_SETUP + '''
    mov ax,350eh
    int 21h
    mov [old_irq6],bx
    mov [old_irq6+2],es
    mov dx,count_irq6
    mov ax,250eh
    int 21h
    push cs
    pop es
    mov dx,filename
    xor cx,cx
    mov ah,3ch
    int 21h
    jc done
    mov [handle],ax
'''
    commands = ''
    for index, case in enumerate(cases):
        if case.position is not None:
            code += f'mov byte [seek_command+2],{case.position}\n' + SEEK
        code += DMA.replace('mov cx,512', 'mov cx,528')
        if case.opcode == 0x45:
            code += 'mov di,buffer\nmov cx,512\nmov al,0a6h\nrep stosb\n'
            code += 'mov al,4ah\nout 0bh,al\n'
        code += f'''
    in al,08h
    mov dx,3f7h
    mov al,{case.rate}
    out dx,al
    mov di,record+11
    call stamp
    mov byte [irq_count],0
    mov si,command_{index}
    mov cx,{2 if case.opcode == 0x4a else 9}
    call send_command
    call wait_result
    call observe
'''
        commands += (f'command_{index} db {case.opcode},0,{case.cylinder},0,'
                     f'{case.sector},2,{case.eot},54h,0ffh\n')
    code += '''
    mov bx,[handle]
    mov ah,3eh
    int 21h
    push ds
    lds dx,[old_irq6]
    mov ax,250eh
    int 21h
    pop ds
    jmp checked
'''
    clock = DECLARATIONS[DECLARATIONS.index('stamp:'):]
    return code + commands + OBSERVE + clock + ROUTINES + 'checked:\n'


def elapsed(first_tick, first_pit, last_tick, last_pit):
    """Convert independent guest PIT observations to milliseconds."""
    return ((last_tick - first_tick) * 65536 + first_pit - last_pit) * 1000 / 1193182


def read_records(path):
    """Require a complete record stream rather than accepting a partial report."""
    data = path.read_bytes()
    assert len(data) % RECORD.size == 0, ('truncated observation', len(data))
    return list(RECORD.iter_unpack(data))


def check_record(case, record, label):
    """Check one observed command without deriving expectations from a backend."""
    status, remaining, terminal, irq, t0, p0, t1, p1, data = record
    context = (label, case.name, status.hex(), remaining, terminal, irq)
    assert tuple(status[:3]) == case.status, context
    assert remaining == (0xffff if case.transferred else 511), context
    assert terminal == (4 if case.transferred else 0), context
    assert irq == 1, context
    assert data[:512] == bytes([case.marker]) * 512, context
    assert data[512:] == b'\xa5' * 16, context
    assert status[3] == (0 if case.name == 'wrong-cylinder' else case.cylinder), context
    assert status[4] == 0 and status[6] == 2, context
    if case.opcode == 0x4a:
        assert 1 <= status[5] <= 9, context
    else:
        assert status[5] == case.sector + int(
            bool(case.transferred) and case.status == (0, 0, 0)), context
    duration = elapsed(t0, p0, t1, p1)
    assert 0 <= duration < 425, (context, duration)
    if case.name in ('wrong-cylinder', 'missing-id'):
        assert duration > 190, (context, 'premature search completion', duration)


def check(cases, records, label):
    """Check the contract and cyclic ID order across a complete revolution."""
    assert len(records) == len(cases), (label, 'record count')
    for case, record in zip(cases, records):
        check_record(case, record, label)
    ids = [record for case, record in zip(cases, records) if case.opcode == 0x4a]
    if ids:
        assert len(ids) == 10, label
        for first, second in zip(ids, ids[1:]):
            assert second[0][5] == first[0][5] % 9 + 1, (label, 'ID order')
        period = elapsed(ids[0][6], ids[0][7], ids[-1][6], ids[-1][7])
        assert 185 < period < 215, (label, 'revolution period', period)


def read_cases(written):
    """Preserve neighbouring sectors and another cylinder around the write."""
    cases = [Case(f'read-{sector}', sector=sector,
                  marker=0xa6 if written and sector == 2 else 0x10 + sector,
                  transferred=512) for sector in (1, 2, 3, 9)]
    cases.append(Case('other-cylinder', sector=2, cylinder=1, position=1,
                      marker=0x62, transferred=512))
    return cases


def check_backend(args, folder, extension):
    """Run common commands, then reopen the disk for persistence and WP checks."""
    disk = source_disk()
    raw = extension in ('img', 'ima')
    original = bytes(disk) if raw else encode_tracks(
        extension, [encode_track(disk, 0), encode_track(disk, 2)])
    image = folder / f'contract.{extension}'
    image.write_bytes(original)
    cases = [Case(f'id-{i}', opcode=0x4a) for i in range(10)]
    cases += [Case('read', marker=0x11, transferred=512),
              Case('wrong-rate', rate=0, status=(0x40, 1, 0)),
              Case('rate-recovery', marker=0x11, transferred=512),
              Case('wrong-cylinder', cylinder=1, status=(0x40, 4, 0x10)),
              Case('missing-id', sector=99, status=(0x40, 4, 0)),
              Case('write', opcode=0x45, sector=2, marker=0xa6, transferred=512)]
    run_guest(args.dosbox.resolve(), args.nasm, folder, image, guest(cases), read_only=False,
              extra_commands=('config -set cpu cpu_cycles=20000',))
    check(cases, read_records(folder / 'CONTRACT.BIN'), extension)
    expected = bytearray(original)
    if raw:
        expected[512:1024] = b'\xa6' * 512
    assert image.read_bytes() == expected, (extension, 'source write extent')
    overlay = Path(str(image) + '.overlay')
    persisted = None if raw else overlay.read_bytes()
    replay = read_cases(True)
    run_guest(args.dosbox.resolve(), args.nasm, folder, image, guest(replay),
              extra_commands=('config -set cpu cpu_cycles=20000',))
    check(replay, read_records(folder / 'CONTRACT.BIN'), extension + ' restart')
    protected = [Case('protected-write', opcode=0x45, sector=2,
                      marker=0xa6, status=(0x40, 2, 0))]
    run_guest(args.dosbox.resolve(), args.nasm, folder, image, guest(protected))
    check(protected, read_records(folder / 'CONTRACT.BIN'), extension + ' protected')
    assert image.read_bytes() == expected, (extension, 'read/protected source mutation')
    assert raw or overlay.read_bytes() == persisted, (extension, 'read/protected overlay mutation')
    print(f'{extension}: shared status/CHRN/IRQ6/DMA/timing, write extent and restart passed',
          flush=True)


# pylint: disable-next=too-many-locals
def check_weak_id(args, folder, duplicate=False):
    """A damaged ID on another turn must not retain the old written payload."""
    for damaged in (False, True):
        image = folder / f'weak-id-{damaged}-{duplicate}.scp'
        turns = [make_track(((1, 0xfb, '', 0x11),), 2),
                 make_track(((1, 0xfb, 'id' if damaged else '', 0x77),), 2)]
        if duplicate:
            turns = [turn[:6250] + make_track(((1, 0xfb, '', 0x33),), 2)[:6250]
                     for turn in turns]
        original = scp_turns(turns)
        image.write_bytes(original)
        write = [Case('write', opcode=0x45, marker=0xa6, transferred=512)]
        run_guest(args.dosbox.resolve(), args.nasm, folder, image, guest(write), read_only=False)
        check(write, read_records(folder / 'CONTRACT.BIN'), 'weak-ID write')
        overlay = Path(str(image) + '.overlay')
        saved = overlay.read_bytes()
        reads = [Case(f'turn-{i}', opcode=0x42, marker=0xa6, transferred=512, eot=1)
                 for i in range(4)]
        if duplicate:
            reads = [Case(f'turn-{i}', opcode=0x46 if i % 2 else 0x42, eot=1)
                     for i in range(4)]
        # Read Track reports the encountered ID, not Read Data's next R.
        run_guest(args.dosbox.resolve(), args.nasm, folder, image, guest(reads))
        records = read_records(folder / 'CONTRACT.BIN')
        assert len(records) == 4
        statuses = []
        for index, (status, remaining, terminal, irq, *_time, data) in enumerate(records):
            assert status[1] in (0, 0x20) and status[2] == 0, status.hex()
            other_copy = duplicate and index % 2
            assert status[3:] == bytes((int(bool(other_copy)), 0, 1, 2)), status.hex()
            assert (remaining, terminal, irq) == (0xffff, 4, 1)
            assert data == bytes([0x33 if other_copy else 0xa6]) * 512 + b'\xa5' * 16
            statuses.append(status[1])
        expected = [0x20, 0, 0 if duplicate else 0x20, 0] if damaged else [0] * 4
        assert statuses == expected, statuses
        assert image.read_bytes() == original
        assert overlay.read_bytes() == saved
    print(f'SCP duplicate={duplicate}: good/bad-ID write arc, CRC and restart passed', flush=True)


def check_capture(args, folder, extension):
    """Capture-only contracts preserve duplicate identity and CRC error data."""
    image = folder / f'duplicates.{extension}'
    # Separate the copies by half a turn so report I/O cannot skip an ID.
    cells = (make_track(((1, 0xfb, '', 0x11),), 2)[:6250] +
             make_track(((1, 0xfb, '', 0x77),), 2)[:6250])
    original = encode_tracks(extension, [cells])
    image.write_bytes(original)
    write = [Case('duplicate-write', opcode=0x45, marker=0xa6, transferred=512)]
    run_guest(args.dosbox.resolve(), args.nasm, folder, image, guest(write), read_only=False)
    check(write, read_records(folder / 'CONTRACT.BIN'), extension + ' duplicate write')
    overlay = Path(str(image) + '.overlay')
    saved = overlay.read_bytes()
    reads = [Case(f'duplicate-{i}', marker=marker, transferred=512)
             for i, marker in enumerate((0xa6, 0x77, 0xa6, 0x77))]
    run_guest(args.dosbox.resolve(), args.nasm, folder, image, guest(reads))
    check(reads, read_records(folder / 'CONTRACT.BIN'), extension + ' duplicate restart')
    assert image.read_bytes() == original
    assert overlay.read_bytes() == saved
    damaged = folder / f'data-crc.{extension}'
    crc_source = encode_tracks(extension, [make_track(((1, 0xfb, 'data', 0x77),), 2)])
    damaged.write_bytes(crc_source)
    crc_case = [Case('data-crc', marker=0x77, status=(0x40, 0x20, 0x20), transferred=512)]
    run_guest(args.dosbox.resolve(), args.nasm, folder, damaged, guest(crc_case))
    check(crc_case, read_records(folder / 'CONTRACT.BIN'), extension + ' CRC')
    assert damaged.read_bytes() == crc_source
    assert not Path(str(damaged) + '.overlay').exists()
    print(f'{extension}: selected duplicate/restart and CRC contract passed', flush=True)


def main():
    """Keep the common contract and capture-only capabilities explicit."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', type=Path, required=True)
    parser.add_argument('--nasm', default='nasm')
    parser.add_argument('--weak-id', action='store_true')
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        folder = Path(temporary)
        if args.weak_id:
            check_weak_id(args, folder)
            check_weak_id(args, folder, True)
            for extension in FORMATS[2:]:
                check_capture(args, folder, extension)
        else:
            for extension in FORMATS:
                check_backend(args, folder, extension)


if __name__ == '__main__':
    main()
