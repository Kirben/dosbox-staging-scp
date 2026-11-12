#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check saved BIOS FDC results, including deleted fields with bad CRCs."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import MFM
from run_long_read_track_probe import make_image
from run_transcopy_layout_probe import run_guest


def encoded_track():
    """Build normal and deleted fields, each with a good and bad CRC."""
    cells = bytearray()
    previous = 0

    def append(data):
        nonlocal previous
        for byte in data:
            cells.extend(MFM[previous][byte])
            previous = byte & 1

    def field(data, bad_crc=False):
        nonlocal previous
        append(bytes(12))
        cells.extend(b"\x44\x89" * 3)
        previous = 1
        crc = crc_hqx(b"\xa1" * 3 + data, 0xffff) ^ int(bad_crc)
        append(data + pack(">H", crc))

    append(b"\x4e" * 80)
    for sector in range(1, 5):
        field(bytes((0xfe, 0, 0, sector, 2)))
        append(b"\x4e" * 22)
        mark = 0xf8 if sector % 2 == 0 else 0xfb
        field(bytes([mark]) + bytes([sector]) * 512, sector >= 3)
        append(b"\x4e" * 40)
    append(b"\x4e" * ((12500 - len(cells)) // 2))
    return cells


def guest_checks():
    """Check all seven result bytes after BIOS Read and Verify."""
    checks = ""
    stage = 0
    for operation in (2, 4):
        for sector in range(1, 5):
            stage += 1
            deleted = sector % 2 == 0
            bad_crc = sector >= 3
            status = 0x10 if bad_crc else (4 if deleted else 0)
            st0 = 0x40 if status else 0
            st1 = 0x20 if bad_crc else 0
            st2 = (0x40 if deleted else 0) | (0x20 if bad_crc else 0)
            # Normal DMA TC at EOT advances the reported cylinder;
            # Verify and abnormal completion retain the current ID.
            result_cylinder = 1 if operation == 2 and not status else 0
            checks += f"""
    mov byte [stage],{stage}
    push es
    xor ax,ax
    mov es,ax
    mov di,442h
    mov al,0a5h
    mov cx,7
    rep stosb
    pop es
    mov ax,{operation * 256 + 1}
    mov bx,buffer
    mov cx,{sector}
    xor dx,dx
    int 13h
    {'jnc' if status else 'jc'} done
    cmp ah,{status}
    jne done
    push ds
    xor ax,ax
    mov ds,ax
    mov si,442h
    mov di,result_{stage}
    mov cx,7
    repe cmpsb
    pop ds
    jne done
"""
            if operation == 2:
                checks += f"""
    mov si,buffer
    mov cx,512
data_{stage}:
    lodsb
    cmp al,{sector}
    jne done
    loop data_{stage}
"""
            checks += f"""
    jmp next_{stage}
result_{stage} db {st0},{st1},{st2},{result_cylinder},0,{sector},2
next_{stage}:
"""
    return checks


def main():
    """Exercise two track containers and standard-PC BIOS configurations."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("tc", "mfm"):
            image = directory / f"results.{extension}"
            original = make_image(encoded_track(), extension)
            image.write_bytes(original)
            for machine in ("cga", "svga_s3"):
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          guest_checks(), machine=machine)
                assert image.read_bytes() == original
                print(f"{machine} .{extension}: BIOS FDC results and deleted CRC passed")


if __name__ == "__main__":
    main()
