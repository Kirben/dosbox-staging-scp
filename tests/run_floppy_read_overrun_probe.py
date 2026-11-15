#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that read overruns consume the sector and retain CRC status."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_contract_probe import Case, guest, read_records, elapsed
from run_floppy_transfer_edges_probe import one_sector
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


# pylint: disable-next=too-many-locals,too-many-statements
def main():
    """Compare normal DMA, starved DMA/FIFO and unserviced scalar PIO."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', type=Path, required=True)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    with TemporaryDirectory(prefix='read-overrun-audit-') as temporary:
        folder = Path(temporary)
        for extension in ('img', 'ima', 'mfm', 'scp', '86f', 'tc'):
            raw = extension in ('img', 'ima')
            for bad_crc in ((False,) if raw else (False, True)):
                image = folder / f'read-{bad_crc}.{extension}'
                original = b'\x77' * (360 * 1024) if raw else encode_tracks(
                    extension, [one_sector(2, bad_crc)])
                image.write_bytes(original)
                full_time = None
                for mode in ('full', 'dma-overrun', 'fifo-overrun', 'pio-overrun'):
                    code = guest([Case('diagnostic-read')])
                    # Match HLT/SRT in every case so the same turn is sampled.
                    setup = '''
        mov dx,3f5h
        mov al,3
        out dx,al
        mov al,0dfh
        out dx,al
        mov al,2
        out dx,al
    '''
                    if mode != 'full':
                        setup += 'mov al,6\nout 0ah,al\n'
                    if mode == 'fifo-overrun':
                        setup += '''
        mov dx,3f5h
        mov al,13h
        out dx,al
        xor al,al
        out dx,al
        out dx,al
        out dx,al
    '''
                    if mode == 'pio-overrun':
                        setup += '''
        mov dx,3f5h
        mov al,3
        out dx,al
        mov al,0dfh
        out dx,al
        mov al,3
        out dx,al
    '''
                    code = code.replace('    mov si,command_0\n', setup + '    mov si,command_0\n')
                    # Do not mistake non-DMA data-ready F0h for result D0h.
                    code = code.replace('and al,0d0h', 'and al,0f0h')
                    run_guest(args.dosbox.resolve(), args.nasm, folder, image, code,
                              extra_commands=('config -set cpu cpu_cycles=20000',))
                    status, count, terminal, irq, t0, p0, t1, p1, data = read_records(
                        folder / 'CONTRACT.BIN')[0]
                    duration = elapsed(t0, p0, t1, p1)
                    crc = 0x20 if bad_crc else 0
                    expected = (0x40 if bad_crc or mode != 'full' else 0,
                                crc | (0x10 if mode != 'full' else 0), crc)
                    assert status[:3] == bytes(expected), (extension, mode, status.hex())
                    assert irq >= 1, (extension, mode, irq)
                    if mode == 'full':
                        full_time = duration
                        assert count == 0xffff and terminal, (extension, mode, count, terminal)
                        assert data[:512] == b'\x77' * 512, (extension, mode, data[:8])
                    else:
                        assert abs(duration - full_time) < 5, (extension, mode, duration, full_time)
                        assert count == 511 and not terminal, (extension, mode, count, terminal)
                        assert data == b'\xa5' * len(data), (extension, mode, data[:8])
                    print(f'{extension} bad-CRC={bad_crc} {mode}: ST={status[:3].hex()} '
                          f'time={elapsed(t0,p0,t1,p1):.3f} ms '
                          f'DMA-left={count+1} TC={terminal} data={data[0]:02x}', flush=True)
                assert image.read_bytes() == original


if __name__ == '__main__':
    main()
