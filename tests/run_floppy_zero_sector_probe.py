#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check BIOS sector-zero commands and subsequent rotational Read ID."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack, pack_into
from tempfile import TemporaryDirectory

from floppy_fixture_cache import cached_fixture

from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest


def encoded_track(cylinder, zero_present):
    """Give cylinder one two nearby IDs followed by a long index gap."""
    cells = bytearray()
    previous = 0

    def append(data):
        nonlocal previous
        for byte in data:
            cells.extend(MFM[previous][byte])
            previous = byte & 1

    def field(data):
        nonlocal previous
        append(bytes(12))
        cells.extend(b"\x44\x89" * 3)
        previous = 1
        append(data + pack(">H", crc_hqx(b"\xa1" * 3 + data, 0xffff)))

    append(b"\x4e" * 80)
    ids = (0, 1, 2) if zero_present else (1, 2)
    if cylinder == 0:
        ids = (9,)
    size_code = 2 if cylinder == 0 else 1
    for sector in ids:
        field(bytes((0xfe, cylinder, 0, sector, size_code)))
        append(b"\x4e" * 22)
        field(b"\xfb" + bytes([0x70 + sector]) * (128 << size_code))
        append(b"\x4e" * 40)
    append(b"\x4e" * ((12500 - len(cells)) // 2))
    return cells


def encode_image(extension, zero_present):
    """Put identical physical tracks into four independent containers."""
    tracks = [encoded_track(cylinder, zero_present) for cylinder in range(2)]
    return encode_tracks(extension, tracks)


@cached_fixture
def encode_tracks(extension, tracks):
    """Wrap single-sided MFM cell streams without changing their contents."""
    cylinders = len(tracks)
    if extension == "mfm":
        image = bytearray(19 + cylinders * 11)
        pack_into("<7sHBHHBI", image, 0, b"HXCMFM", cylinders, 1, 300, 250, 7, 19)
    elif extension == "86f":
        image = bytearray(8 + 256 * 4)
        pack_into("<4sHH", image, 0, b"86BF", 0x020c, 0x1881)
    elif extension == "scp":
        image = bytearray(0x2b0)
        image[:9] = b"SCP" + bytes((0x10, 0x31, 1, 0, (cylinders - 1) * 2, 1))
    else:
        image = bytearray(0xa00)
        image[:2] = b"\x5a\xa5"
        image[0x100:0x105] = bytes((7, 0, cylinders - 1, 1, 1))
    for cylinder, cells in enumerate(tracks):
        if extension == "mfm":
            pack_into("<HBII", image, 19 + cylinder * 11,
                      cylinder, 0, len(cells), len(image))
            image.extend(cells)
        elif extension == "86f":
            pack_into("<I", image, 8 + cylinder * 4, len(image))
            image.extend(pack("<HiI", 0x0a, len(cells) * 8, 0))
            swapped = bytearray(len(cells))
            swapped[0::2], swapped[1::2] = cells[1::2], cells[0::2]
            image.extend(swapped + bytes(len(cells)))
        elif extension == "scp":
            bits = "".join(f"{byte:08b}" for byte in cells)
            edges = [i + 1 for i, bit in enumerate(bits) if bit == "1"]
            flux = b"".join(pack(">H", (edge - prior) * 80)
                            for edge, prior in zip(edges, [0] + edges[:-1]))
            track = cylinder * 2
            pack_into("<I", image, 16 + track * 4, len(image))
            image.extend(pack("<3sBIII", b"TRK", track, len(bits) * 80,
                              len(flux) // 2, 16) + flux)
        else:
            entry = cylinder * 2
            pack_into("<H", image, 0x105 + entry * 2, 0x1111)
            pack_into(">H", image, 0x305 + entry * 2, len(image) // 256)
            pack_into("<H", image, 0x505 + entry * 2, len(cells))
            image.extend(cells)
            image.extend(bytes(-len(image) % 256))
    if extension == "scp":
        pack_into("<I", image, 12, sum(image[16:]) & 0xffffffff)
    return image


def guest_checks(unit, operation, zero_present, written=False):
    """Check status, count, IRQ6, DMA, buffer and the selected cylinder."""
    status = 0 if zero_present else 4
    completed = 1 if zero_present else 0
    data = 0x70 if zero_present and operation == 2 and not written else 0xa5
    dma_count = 0xffff if zero_present else 0xff
    repeat_check = "" if zero_present else "cmp al,[first]\n    jne failure"
    dma_check = "" if operation == 4 else f"""
    out 0ch,al
    in al,05h
    cmp al,{dma_count & 255}
    jne failure
    in al,05h
    cmp al,{dma_count >> 8}
    jne failure
    in al,08h
    and al,4
    cmp al,{4 if zero_present else 0}
    jne failure
"""
    readback = "" if operation != 3 or not zero_present else f"""
    mov ax,0201h
    mov bx,buffer
    mov cx,0100h
    mov dx,{unit}
    int 13h
    jc failure
    cmp ax,1
    jne failure
    cmp word [irq_count],2
    jne failure
    mov si,buffer
    mov cx,256
check_written:
    lodsb
    cmp al,0a5h
    jne failure
    loop check_written
"""
    return f"""
    mov ax,0201h
    mov bx,buffer
    mov cx,9
    mov dx,{unit}
    int 13h
    jc done
    push es
    xor ax,ax
    mov es,ax
    les bx,[es:78h]
    mov byte [es:bx+3],1
    pop es
    mov ax,350eh
    int 21h
    mov [old_irq],bx
    mov [old_irq+2],es
    push cs
    pop es
    mov dx,irq_handler
    mov ax,250eh
    int 21h
    mov di,buffer
    mov al,0a5h
    mov cx,512
    rep stosb
    mov ax,{operation * 256 + 1}
    mov bx,buffer
    mov cx,0100h
    mov dx,{unit}
    int 13h
    {'jc' if zero_present else 'jnc'} failure
    cmp ah,{status}
    jne failure
    cmp al,{completed}
    jne failure
    inc byte [stage]
    cmp word [irq_count],1
    jne failure
    mov si,buffer
    mov cx,256
check_data:
    lodsb
    cmp al,{data}
    jne failure
    loop check_data
    mov cx,256
check_guard:
    lodsb
    cmp al,0a5h
    jne failure
    loop check_guard
    inc byte [stage]
    {dma_check}
    {readback}
    inc byte [stage]
    call read_id
    jc failure
    mov al,[result+5]
    mov [first],al
    call read_id
    jc failure
    mov al,[result+5]
    cmp al,[first]
    je failure
    call read_id
    jc failure
    mov al,[result+5]
    {repeat_check}
    call restore_irq
    jmp checked
failure:
    call restore_irq
    jmp done
restore_irq:
    push ds
    lds dx,[old_irq]
    mov ax,250eh
    int 21h
    pop ds
    ret
read_id:
    inc byte [stage]
    mov bp,[irq_count]
    mov dx,3f5h
    mov al,4ah
    out dx,al
    mov al,{unit}
    out dx,al
    push ds
    xor ax,ax
    mov ds,ax
    mov si,[46ch]
wait_result:
    mov dx,3f4h
    in al,dx
    and al,0d0h
    cmp al,0d0h
    jne wait_tick
    cmp [cs:irq_count],bp
    jne result_ready
wait_tick:
    mov ax,[46ch]
    sub ax,si
    cmp ax,18
    jb wait_result
    pop ds
    stc
    ret
result_ready:
    pop ds
    inc bp
    cmp [irq_count],bp
    jne bad_id
    mov dx,3f5h
    mov di,result
    mov cx,7
read_result:
    in al,dx
    stosb
    loop read_result
    cmp byte [result],{unit}
    jne bad_id
    cmp word [result+1],0
    jne bad_id
    cmp word [result+3],1
    jne bad_id
    cmp byte [result+6],1
    jne bad_id
    clc
    ret
bad_id:
    stc
    ret
irq_handler:
    inc word [cs:irq_count]
    jmp far [cs:old_irq]
old_irq dd 0
irq_count dw 0
first db 0
result times 7 db 0
checked:
"""


def main():
    """Exercise missing and recorded sector zero on both floppy units."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("tc", "mfm", "scp", "86f"):
            image = directory / f"zero.{extension}"
            for zero_present in (False, True):
                original = encode_image(extension, zero_present)
                image.write_bytes(original)
                second = directory / f"second.{extension}"
                second.write_bytes(original)
                for unit in (0, 1):
                    mount = f'mount 1 "{second}" -t floppy -fs none'
                    for operation in (2, 4, 3):
                        print(f".{extension}: zero={zero_present}, "
                              f"unit={unit}, AH={operation:02x}", flush=True)
                        run_guest(args.dosbox.resolve(), args.nasm, directory,
                                  image, guest_checks(unit, operation, zero_present),
                                  extra_commands=(mount,), read_only=False)
                if zero_present:
                    for unit in (0, 1):
                        run_guest(args.dosbox.resolve(), args.nasm, directory,
                                  image, guest_checks(unit, 2, True, written=True),
                                  extra_commands=(mount,), read_only=False)
                assert image.read_bytes() == original
                assert second.read_bytes() == original
                print(f".{extension}: zero present={zero_present}, A/B transfers passed")


if __name__ == "__main__":
    main()
