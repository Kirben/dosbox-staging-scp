#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep SCP sector fields continuous across recorded index boundaries."""
import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack, pack_into
from tempfile import TemporaryDirectory

from run_floppy_86f_surface_probe import read_samples
from run_floppy_read_deleted_probe import make_track
from run_floppy_transfer_edges_probe import RESET, DMA, program
from run_transcopy_layout_probe import run_guest

SYNC = b"\x44\x89" * 3

def oracle(cells):
    """Validate full-stream CRCs at arbitrary cell alignment."""
    bits = ''.join(f'{byte:08b}' for byte in cells)
    repeated = bits + bits
    pattern = '0100010010001001' * 3
    fields = []
    offset = 0
    while True:
        offset = bits.find(pattern, offset)
        if offset < 0:
            break
        def decoded(position):
            return int(repeated[position:position+16][1::2], 2)
        if decoded(offset + 48) == 0xfb:
            data = bytes(decoded(offset + 64 + 16*i) for i in range(512))
            recorded = decoded(offset + 8256)*256 + decoded(offset + 8272)
            assert recorded == crc_hqx(b'\xa1'*3 + b'\xfb' + data, 0xffff)
            assert len(set(data)) == 1
            fields.append(data[0])
        offset += 1
    assert len(fields) == 2
    return fields

def scp_image(cells):
    """Encode a continuous flux sequence into two indexed SCP descriptors."""
    turns = []
    last = 0
    turn_bytes = len(cells) // 2
    for turn in range(2):
        flux = bytearray()
        for offset, value in enumerate(cells[turn*turn_bytes:(turn+1)*turn_bytes]):
            for bit in range(8):
                if value & (0x80 >> bit):
                    edge = (turn*turn_bytes + offset)*8 + bit + 1
                    flux.extend(pack('>H', (edge-last)*80))
                    last = edge
        turns.append(flux)
    image = bytearray(0x2b0)
    image[:11] = b'SCP' + bytes((0x10, 0x31, 2, 0, 0, 0x81, 0, 1))
    pack_into('<I', image, 16, len(image))
    image.extend(b'TRK\0')
    relative = 28
    for flux in turns:
        image.extend(pack('<III', turn_bytes*8*80, len(flux)//2, relative))
        relative += len(flux)
    for flux in turns:
        image.extend(flux)
    pack_into('<I', image, 12, sum(image[16:]) & 0xffffffff)
    return image

def make_capture(varied, crossing, marker=0x12, index_shift_bits=0):
    """Rotate the entire capture, keeping fields intact across the physical indexes."""
    first = make_track(((1, 0xfb, "", marker),), 2)
    second = make_track(((1, 0xfb, "", 0x24 if varied else marker),), 2)
    cells = first + second
    if crossing:
        data_sync = first.find(SYNC, first.find(SYNC) + 6)
        shift = data_sync + 8 + 256
        cells = cells[shift:] + cells[:shift]
    if index_shift_bits:
        bits = ''.join(f'{byte:08b}' for byte in cells)
        bits = bits[index_shift_bits:] + bits[:index_shift_bits]
        cells = bytearray(int(bits[i:i+8], 2) for i in range(0, len(bits), 8))
    markers = oracle(cells)
    assert set(markers) == ({marker, 0x24} if varied else {marker})
    return scp_image(cells)


def check_direct(dosbox, nasm, directory, image, marker):
    """Read full fields through direct DMA Read Track as well as BIOS Read Data."""
    checks = RESET
    for index in range(2):
        checks += DMA + f"""
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    mov bl,[buffer]
    cmp bl,{marker}
    je valid_{index}
    cmp bl,24h
    jne done
valid_{index}:
    mov si,buffer
    mov cx,512
compare_{index}:
    lodsb
    cmp al,bl
    jne done
    loop compare_{index}
"""
    run_guest(dosbox, nasm, directory, image,
              program(checks, "command db 42h,0,0,0,1,2,1,54h,0ffh\n"),
              extra_commands=("config -set cpu cpu_cycles=20000",))


# pylint: disable-next=too-many-locals
def main():
    """Check index-in-gap and constant-data controls, then varying cross-index data."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--exact-index", action="store_true")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        cases = (("varied-gap", True, False, 0x12, 0),
                 ("constant-crossing", False, True, 0x12, 0),
                 ("varied-crossing", True, True, 0x12, 0))
        if args.exact_index:
            cases = (("exact-index-transition", False, True, 0x11, 0),
                     ("index-between-transitions", False, True, 0x11, 1))
        for name, varied, crossing, marker, phase in cases:
            image = directory / f"{name}.scp"
            original = make_capture(varied, crossing, marker, phase)
            image.write_bytes(original)
            samples, statuses, carry = read_samples(
                args.dosbox.resolve(), args.nasm, directory, image)
            assert statuses == carry == [0] * 4
            expected = {bytes([marker]) * 512}
            if varied:
                expected.add(bytes([0x24]) * 512)
            assert set(samples) == expected
            if varied:
                assert all(first != second for first, second in zip(samples, samples[1:]))
            check_direct(args.dosbox.resolve(), args.nasm, directory, image, marker)
            samples, statuses, carry = read_samples(
                args.dosbox.resolve(), args.nasm, directory, image, action="write")
            assert samples == [b"\x66" * 512] * 4
            assert statuses == carry == [0] * 4
            overlay = Path(str(image) + ".overlay")
            saved = overlay.read_bytes()
            samples, statuses, carry = read_samples(
                args.dosbox.resolve(), args.nasm, directory, image)
            assert samples == [b"\x66" * 512] * 4
            assert statuses == carry == [0] * 4
            assert overlay.read_bytes() == saved
            assert image.read_bytes() == original
            print(f"{name}: continuous CRC, BIOS/FDC reads and persistent writes passed",
                  flush=True)


if __name__ == "__main__":
    main()
