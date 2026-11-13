#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check FM marks/CRCs, encoding selection, rotational IDs and byte timing."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack, pack_into, unpack_from
from tempfile import TemporaryDirectory

from run_floppy_transfer_edges_probe import DMA, RESET, ROUTINES
from run_floppy_fifo_probe import COUNT, DELAY
from run_floppy_format_index_probe import CLOCK_SETUP
from run_floppy_read_deleted_probe import make_track
from run_hxc_mfm_probe import MFM
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def fm_byte(value, clock=0xff):
    """Interleave the FM clock byte with its data byte."""
    word = 0
    for bit in range(7, -1, -1):
        word = (word << 2) | (((clock >> bit) & 1) << 1) | ((value >> bit) & 1)
    return pack(">H", word)


def fm_track(size_code, error="", deleted=False):
    """Give one turn two identical IDs with distinct payloads and valid FM clocks."""
    cells = bytearray(fm_byte(0xff) * 40)
    for marker in (0x11, 0x22):
        cells.extend(fm_byte(0) * 6 + fm_byte(0xfe, 0xc7))
        header = bytes((0, 0, 1, size_code))
        crc = crc_hqx(b"\xfe" + header, 0xffff) ^ (error == "id")
        for value in header + pack(">H", crc):
            cells.extend(fm_byte(value))
        cells.extend(fm_byte(0xff) * 11 + fm_byte(0) * 6)
        mark = 0xf8 if deleted else 0xfb
        cells.extend(fm_byte(mark, 0xc7))
        payload = bytes([marker]) * (128 << size_code)
        crc = crc_hqx(bytes([mark]) + payload, 0xffff) ^ (error == "data")
        for value in payload + pack(">H", crc):
            cells.extend(fm_byte(value))
        cells.extend(fm_byte(0xff) * 48)
    cells.extend(fm_byte(0xff) * ((6250 - len(cells)) // 2))
    return cells


def fm_image(extension, cells):
    """Store a 125-kbps FM track in each captured-track container."""
    if extension == "tc":
        bits = "".join(f"{byte:08b}" for byte in cells)
        expanded = "".join("0" + bit for bit in bits)
        cells = bytes(int(expanded[i:i + 8], 2) for i in range(0, len(expanded), 8))
    image = bytearray(encode_tracks(extension, [cells]))
    if extension == "86f":
        record = unpack_from("<I", image, 8)[0]
        pack_into("<H", image, record, 2)
    elif extension == "mfm":
        pack_into("<H", image, 12, 125)
    elif extension == "scp":
        record = unpack_from("<I", image, 16)[0]
        duration, count, offset = unpack_from("<III", image, record + 4)
        pack_into("<I", image, record + 4, duration * 2)
        for position in range(record + offset, record + offset + count * 2, 2):
            value = unpack_from(">H", image, position)[0]
            pack_into(">H", image, position, value * 2)
        pack_into("<I", image, 12, sum(image[16:]) & 0xffffffff)
    return image


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check_image(dosbox, nasm, directory, extension, size_code, error="", deleted=False):
    """Read FM records, reject MFM selection, recover and preserve the source."""
    image = directory / f"fm-{size_code}-{error}-{deleted}.{extension}"
    original = fm_image(extension, fm_track(size_code, error, deleted))
    image.write_bytes(original)
    size = 128 << size_code
    checks = RESET
    commands = ""
    for index, (mfm, opcode) in enumerate(((False, 0x0a), (True, 0x4a),
                                         (False, 0x0a), (False, 0x0c if deleted else 0x06),
                                         (False, 0x02), (False, 0x16))):
        checks += DMA.replace("mov cx,512", "mov cx,1040").replace(
            "mov al,0ffh\n    out 05h,al", f"mov al,{(size - 1) & 255}\n    out 05h,al").replace(
            "mov al,1\n    out 05h,al", f"mov al,{(size - 1) >> 8}\n    out 05h,al")
        count = 2 if opcode in (0x0a, 0x4a) else 9
        commands += (f"cmd{index} db {opcode},0\n" if count == 2 else
                     f"cmd{index} db {opcode},0,0,0,1,{size_code},1,54h,0ffh\n")
        checks += f"mov byte [stage],{index + 1}\nmov si,cmd{index}\nmov cx,{count}\n"
        checks += "call send_command\ncall wait_result\n"
        expected_1 = 1 if mfm else 0x20 if error == "id" or (
            error == "data" and count == 9) else 0
        if opcode in (2, 0x16) and deleted:
            expected_2 = 0x40
        else:
            expected_2 = 0
        if error == "data" and count == 9:
            expected_2 |= 0x20
        checks += f"cmp byte [ids+1],{expected_1}\njne done\n"
        checks += f"cmp byte [ids+2],{expected_2}\njne done\n"
        if not mfm:
            checks += f"cmp byte [ids+5],1\njne done\ncmp byte [ids+6],{size_code}\njne done\n"
        data = count == 9 and opcode != 0x16 and (error != "id" or opcode == 2)
        if data:
            checks += f"""
    mov al,[buffer]
    cmp al,11h
    je valid{index}
    cmp al,22h
    jne done
valid{index}:
    mov di,buffer
    mov cx,{size}
    repe scasb
    jne done
    mov cx,16
guard{index}:
    cmp byte [di],0a5h
    jne done
    inc di
    loop guard{index}
"""
        else:
            checks += f"mov di,buffer\nmov cx,{size}\nmov al,0a5h\nrepe scasb\njne done\n"
    run_guest(dosbox, nasm, directory, image,
              checks + "jmp checked\n" + commands + ROUTINES + "checked:\n",
              extra_commands=("config -set cpu cpu_cycles=20000",))
    assert image.read_bytes() == original
    assert not Path(str(image) + ".overlay").exists()
    print(f".{extension} FM N={size_code} {error or 'valid'} deleted={deleted}: "
          "ID/data/track/verify, MFM rejection, CRC and guards passed", flush=True)


def main():
    """Exercise FM records and encoding mismatches across captured formats."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        for extension in ("scp", "mfm", "86f", "tc"):
            for size_code in (0, 1, 2):
                check_image(args.dosbox.resolve(), args.nasm, Path(temp), extension, size_code)
            for error, deleted in (("id", False), ("data", False), ("", True)):
                check_image(args.dosbox.resolve(), args.nasm, Path(temp), extension, 1,
                            error, deleted)
            check_selection(args.dosbox.resolve(), args.nasm, Path(temp), extension)
            check_timing(args.dosbox.resolve(), args.nasm, Path(temp), extension)


# pylint: disable-next=too-many-locals
def check_selection(dosbox, nasm, directory, extension):
    """Select FM or MFM fields on one track, reject wrong rates and FM writes."""
    fm = fm_track(2)[:3000]
    bits = "".join(f"{byte:08b}" for byte in fm)
    expanded = "".join("0" + bit for bit in bits)
    cells = bytearray(int(expanded[i:i + 8], 2) for i in range(0, len(expanded), 8))
    cells.extend(make_track(((3, 0xfb, "", 0x33),), 2)[:1600])
    cells.extend(MFM[0][0x4e] * ((12500 - len(cells)) // 2))
    image = directory / f"selection.{extension}"
    original = encode_tracks(extension, [cells])
    image.write_bytes(original)
    checks, commands = RESET, ""
    # Both encodings use selector 2, despite their different byte rates.
    for index, (opcode, sector, selector, success) in enumerate(
            ((0x06, 1, 2, True), (0x46, 3, 2, True), (0x06, 1, 0, False),
             (0x06, 1, 2, True), (0x05, 1, 2, False), (0x09, 1, 2, False),
             (0x0d, 1, 2, False), (0x46, 3, 2, True))):
        checks += DMA + f"""
    mov byte [stage],{index + 1}
    mov dx,3f7h
    mov al,{selector}
    out dx,al
    mov si,select_{index}
    mov cx,{6 if opcode == 0x0d else 9}
    call send_command
    call wait_result
    cmp byte [ids+1],{0 if success else 1 if selector == 0 else 4}
    jne done
    cmp byte [ids+2],0
    jne done
    mov di,buffer
    mov cx,512
    mov al,{0x11 if sector == 1 else 0x33}
"""
        if success:
            checks += f"""
    cmp byte [buffer],11h
    je payload_{index}
    cmp byte [buffer],22h
    jne other_{index}
    mov al,22h
    jmp payload_{index}
other_{index}:
    cmp byte [buffer],33h
    jne done
payload_{index}:
    repe scasb
    jne done
"""
        else:
            checks += "mov al,0a5h\nrepe scasb\njne done\n"
        commands += (f"select_{index} db {opcode},0,2,1,54h,0e5h\n" if opcode == 0x0d else
                     f"select_{index} db {opcode},0,0,0,{sector},2,{sector},54h,0ffh\n")
    run_guest(dosbox, nasm, directory, image,
              checks + "jmp checked\n" + commands + ROUTINES + "checked:\n",
              read_only=False, extra_commands=("config -set cpu cpu_cycles=20000",))
    assert image.read_bytes() == original
    assert not Path(str(image) + ".overlay").exists()
    print(f".{extension}: mixed FM/MFM, rate selection, recovery and FM write rejection passed",
          flush=True)


def check_timing(dosbox, nasm, directory, extension):
    """Time the live DMA counter over an FM sector, independently of IRQ polling."""
    image = directory / f"timing.{extension}"
    original = fm_image(extension, fm_track(2))
    image.write_bytes(original)
    checks = CLOCK_SETUP + RESET + DMA + """
    mov si,transfer
    mov cx,9
    call send_command
    mov bp,100
    xor cx,cx
first_byte:
    call dma_count
    cmp ax,510
    jbe started
    loop first_byte
    dec bp
    jnz first_byte
    jmp done
started:
    mov byte [stage],2
    call pit_count
    mov [start],ax
    xor cx,cx
last_byte:
    call dma_count
    cmp ax,0ffffh
    je transferred
    loop last_byte
    jmp done
transferred:
    mov byte [stage],3
    call pit_count
    mov dx,[start]
    sub dx,ax
    cmp dx,36000
    jb done
    cmp dx,42000
    ja done
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    jmp checked
transfer db 6,0,0,0,1,2,1,54h,0ffh
start dw 0
""" + COUNT + DELAY + ROUTINES + "checked:\n"
    run_guest(dosbox, nasm, directory, image, checks,
              extra_commands=("config -set cpu cpu_cycles=20000",))
    assert image.read_bytes() == original
    print(f".{extension}: 512 FM DMA bytes took 30–35 ms at selector 250, IRQ6 passed",
          flush=True)


if __name__ == "__main__":
    main()
