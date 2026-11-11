#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check command-sized Read Track fields across IDs and index."""

import argparse
from bisect import bisect_left, bisect_right
from binascii import crc_hqx
from pathlib import Path
from struct import pack, pack_into
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import MFM, encode_track
from run_transcopy_layout_probe import run_guest


CHECKS = """
%ifdef WRITE
    mov di,buffer
    mov cx,512
    mov al,66h
    cld
    rep stosb
    mov ax,0301h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
%endif
    mov dx,3f2h
    xor al,al
    out dx,al
    mov al,1ch
    out dx,al
    mov cx,4
drain_reset:
    mov al,8
    call send
    call receive
    call receive
    loop drain_reset
    mov ax,350eh
    int 21h
    mov [old_vector],bx
    mov [old_vector+2],es
    push cs
    pop es
    mov dx,irq_handler
    mov ax,250eh
    int 21h
    mov al,6
    out 0ah,al
    in al,08h
    mov ax,6000h
    mov es,ax
    xor di,di
    mov cx,LENGTH+16
    mov al,0cch
    cld
    rep stosb
%ifdef PIO
    mov al,3
    call send
    mov al,0dfh
    call send
    mov al,3
    call send
    mov dx,3f7h
    mov al,2
    out dx,al
%else
    xor al,al
    out 0ch,al
    out 04h,al
    out 04h,al
    mov ax,LENGTH-1
    out 05h,al
    mov al,ah
    out 05h,al
    mov al,6
    out 81h,al
    mov al,46h
    out 0bh,al
    mov al,2
    out 0ah,al
%endif
    mov byte [stage],2
    xor ah,ah
    int 1ah
    mov [started],dx
    mov si,command
    mov cx,9
send_command:
    lodsb
    call send
    loop send_command
%ifdef PIO
    xor di,di
    mov cx,LENGTH
read_pio:
    call receive
    stosb
    loop read_pio
%else
wait_irq:
    cmp word [irqs],0
    jne got_irq
    xor ah,ah
    int 1ah
    sub dx,[started]
    cmp dx,36
    jb wait_irq
    jmp done
got_irq:
    xor ah,ah
    int 1ah
    sub dx,[started]
%if BLOCKS > 1
    cmp dx,9
%else
    cmp dx,4
%endif
    jb done
    cmp dx,20
    ja done
    cmp word [irqs],1
    jne done
%endif
    mov byte [stage],3
    mov di,result
    mov cx,7
read_status:
    call receive
    mov [di],al
    inc di
    loop read_status
    mov al,[result+1]
    and al,25h
    cmp al,24h
    jne done
    mov al,[result+2]
    and al,21h
    cmp al,20h
    jne done
    in al,08h
%ifdef PIO
    test al,4
    jnz done
%else
    test al,4
    jz done
    mov al,6
    out 0ah,al
%endif
    mov byte [stage],4
    mov si,expected
    xor di,di
    mov cx,LENGTH
    repe cmpsb
    jne done
    cmp byte [es:LENGTH],0cch
    jne done
    push ds
    lds dx,[old_vector]
    mov ax,250eh
    int 21h
    pop ds
    jmp checked
send:
    push ax
    push dx
    mov dx,3f4h
send_ready:
    in al,dx
    and al,0c0h
    cmp al,80h
    jne send_ready
    pop dx
    pop ax
    push dx
    mov dx,3f5h
    out dx,al
    pop dx
    ret
receive:
    push dx
    mov dx,3f4h
receive_ready:
    in al,dx
    and al,0c0h
    cmp al,0c0h
    jne receive_ready
    inc dx
    in al,dx
    pop dx
    ret
irq_handler:
    push ax
    inc word [cs:irqs]
    mov al,20h
    out 20h,al
    pop ax
    iret
command db 42h,0,0,0,1,6,BLOCKS,2ah,0ffh
old_vector dd 0
started dw 0
irqs dw 0
result times 7 db 0
checked:
"""


def make_image(cells, extension):
    """Store the same encoded track in two independent containers."""
    if extension == "mfm":
        image = bytearray(30)
        pack_into("<7sHBHHBI", image, 0, b"HXCMFM", 1, 1, 300, 250, 7, 19)
        pack_into("<HBII", image, 19, 0, 0, len(cells), len(image))
    else:
        image = bytearray(0xa00)
        image[:2] = b"\x5a\xa5"
        image[0x100:0x105] = bytes((7, 0, 0, 1, 1))
        pack_into("<H", image, 0x105, 0x1111)
        pack_into(">H", image, 0x305, len(image) // 256)
        pack_into("<H", image, 0x505, len(cells))
    image.extend(cells)
    return image


def expected_stream(cells, start=None):
    """Decode from the first DAM, including following records and index."""
    bits = "".join(f"{byte:08b}" for byte in cells)
    ring = bits * 4
    sync = f"{0x4489:016b}" * 3
    if start is None:
        start = 0
        while True:
            mark = ring.index(sync, start)
            kind = int(ring[mark + 48:mark + 64][1::2], 2)
            if kind == 0xfb:
                start = mark + 64
                break
            start = mark + 1
    marks = []
    for pattern in (sync, f"{0x5224:016b}" * 3):
        cursor = 0
        while (cursor := ring.find(pattern, cursor)) >= 0:
            marks.append(cursor)
            cursor += 1
    marks.sort()
    data = bytearray()
    for _ in range(8194):
        next_mark = bisect_left(marks, start)
        if next_mark < len(marks) and marks[next_mark] < start + 16:
            start = marks[next_mark]
        data.append(int(ring[start:start + 16][1::2], 2))
        start += 16
    assert crc_hqx(b"\xa1" * 3 + b"\xfb" + data[:8192], 0xffff) != int.from_bytes(
        data[8192:], "big")
    assert data[:8192].count(data[:512]) >= 2
    return data[:8192]


def two_fields(cells):
    """Locate the next physical ID after a long field has crossed index."""
    bits = "".join(f"{byte:08b}" for byte in cells)
    ring = bits * 4
    sync = f"{0x4489:016b}" * 3
    ids = []
    fields = []
    cursor = 0
    while (cursor := ring.find(sync, cursor, len(bits))) >= 0:
        kind = int(ring[cursor + 48:cursor + 64][1::2], 2)
        if kind == 0xfe:
            ids.append(cursor)
        elif kind == 0xfb:
            fields.append(cursor + 64)
        cursor += 1
    end = (fields[0] + (8192 + 2) * 16) % len(bits)
    next_id = bisect_right(ids, end) % len(ids)
    assert next_id != 1  # A long field must skip IDs passed during the read.
    return expected_stream(cells) + expected_stream(cells, fields[next_id])


def encoded_cells(disk):
    """Include a C2 index mark before the A1 sector marks."""
    prefix = bytearray()
    previous = 0
    for byte in b"\x4e" * 40 + bytes(12):
        prefix.extend(MFM[previous][byte])
        previous = byte & 1
    prefix.extend(b"\x52\x24" * 3)
    previous = 0
    for byte in b"\xfc" + b"\x4e" * 24:
        prefix.extend(MFM[previous][byte])
        previous = byte & 1
    cells = encode_track(disk, 0)
    cells[:len(prefix)] = prefix
    return cells + b"\x92"


def main():
    """Exercise full DMA, early terminal count, PIO and overlay replay."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    disk = bytearray(360 * 1024)
    for sector in range(9):
        disk[sector * 512:(sector + 1) * 512] = bytes([0x31 + sector]) * 512
    # An odd byte count makes the sampling phase change across index.
    cells = encoded_cells(disk)
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("tc", "mfm"):
            image = directory / f"disk.{extension}"
            original = make_image(cells, extension)
            image.write_bytes(original)
            expected = expected_stream(cells)
            for length, pio in ((8192, False), (1024, False), (8192, True), (16384, False)):
                blocks = 2 if length == 16384 else 1
                options = f"%define LENGTH {length}\n%define BLOCKS {blocks}\n"
                options += "%define PIO\n" if pio else ""
                payload = two_fields(cells) if blocks == 2 else expected[:length]
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          options + CHECKS, payload)
                print(f".{extension}: {length} bytes {'PIO' if pio else 'DMA'}, "
                      "payload, CRC, IRQ6 and timing passed")
            disk[:512] = b"\x66" * 512
            updated = expected_stream(encoded_cells(disk))
            for write in (True, False):
                options = "%define LENGTH 8192\n%define BLOCKS 1\n"
                options += "%define WRITE\n" if write else ""
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          options + CHECKS, updated, read_only=not write)
                assert image.read_bytes() == original
            disk[:512] = b"\x31" * 512
            print(f".{extension}: long Read Track reflects writes after restart")


if __name__ == "__main__":
    main()
