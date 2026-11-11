#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check physical TransCopy table slots through guest BIOS reads."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack, pack_into
import subprocess
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import MFM, launch
from run_transcopy_attachment_probe import encode_transcopy


def run_guest(dosbox, nasm, directory, image, checks, expected=b"", extra_commands=()):
    """Run BIOS checks and report the first failing stage."""
    source = directory / "probe.asm"
    (directory / "EXPECTED.BIN").write_bytes(expected)
    source.write_text("""
bits 16
org 100h
    push cs
    pop ds
    push cs
    pop es
""" + checks + """
    mov byte [stage],0
done:
    mov dx,report
    xor cx,cx
    mov ah,3ch
    int 21h
    jc exit
    mov bx,ax
    mov dx,stage
    mov cx,1
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
exit:
    mov ax,4c00h
    int 21h
stage db 1
report db 'RESULT.BIN',0
align 16
buffer times 8192 db 0
expected incbin 'EXPECTED.BIN'
""", encoding="ascii")
    subprocess.run([nasm, "-f", "bin", source, "-o", directory / "PROBE.COM"],
                   cwd=directory, check=True)
    report = directory / "RESULT.BIN"
    report.unlink(missing_ok=True)
    command, env = launch(dosbox, directory, "layout", [
        f'mount c "{directory}"', f'mount 0 "{image}" -t floppy -fs none -ro',
        *extra_commands, "c:", "probe.com", "exit",
    ])
    run = subprocess.run(command, cwd=directory, env=env, timeout=30,
                         capture_output=True, text=True, check=True)
    result = report.read_bytes() if report.exists() else b""
    if result != b"\0":
        raise RuntimeError(f"{image.name}: stage {result.hex()}\n"
                           f"{run.stdout}{run.stderr}")


def check_mapping(dosbox, nasm, directory):
    """Read distant cylinders on native single- and double-sided tables."""
    disk = bytearray(360 * 1024)
    checks = ""
    for stage, cylinder in enumerate((2, 38), 1):
        offset = cylinder * 2 * 9 * 512
        disk[offset:offset + 512] = bytes([cylinder]) * 512
        checks += f"""
    mov byte [stage],{stage}
    mov ax,0201h
    mov bx,buffer
    mov cx,{cylinder * 256 + 1}
    xor dx,dx
    int 13h
    jc done
    cmp ah,0
    jne done
    mov si,buffer
    mov cx,512
check_{stage}:
    lodsb
    cmp al,{cylinder}
    jne done
    loop check_{stage}
"""
    for heads in (1, 2):
        for step in (1, 2):
            image = directory / f"mapping-{heads}-{step}.tc"
            original = encode_transcopy(disk, heads=heads, step=step)
            image.write_bytes(original)
            run_guest(dosbox, nasm, directory, image, checks)
            assert image.read_bytes() == original
            print(f"TC {heads} head(s), step {step}: cylinders 2 and 38 passed")


def encode_field_fixture(oversized):
    """Build an overlapping field or a field longer than one revolution."""
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
        start = len(cells) + 2
        append(data + pack(">H", crc_hqx(b"\xa1" * 3 + data, 0xffff)))
        return start

    append(b"\x4e" * 80)
    field(bytes((0xfe, 0, 0, 1, 6 if oversized else 3)))
    append(b"\x4e" * 22)
    if oversized:
        # The FDC reads through index into this same physical track again.
        start = field(b"\xfb" + b"\x55" * 8192)
        del cells[12500:]
        ring = cells * 3
        expected = bytes(int(f"{int.from_bytes(ring[i:i + 2], 'big'):016b}"[1::2], 2)
                         for i in range(start, start + 8192 * 2, 2))
        recorded = bytes(int(f"{int.from_bytes(ring[i:i + 2], 'big'):016b}"[1::2], 2)
                         for i in range(start + 8192 * 2, start + 8194 * 2, 2))
        assert crc_hqx(b"\xa1" * 3 + b"\xfb" + expected, 0xffff) != int.from_bytes(
            recorded, "big")
    else:
        payload = bytearray(b"\x33" * 1024)
        inner_id = bytes((0xfe, 0, 0, 0xbb, 2))
        inner_data = b"\xfb" + b"\x77" * 512
        for offset, data in ((128, inner_id), (180, inner_data)):
            record = b"\xa1" * 3 + data + pack(">H", crc_hqx(b"\xa1" * 3 + data, 0xffff))
            payload[offset:offset + len(record)] = record
        start = field(b"\xfb" + payload)
        # Missing-clock A1s make both embedded records genuine address marks.
        for offset in (128, 180):
            cells[start + 2 * offset:start + 2 * offset + 6] = b"\x44\x89" * 3
        expected = bytes(payload)
        append(b"\x4e" * ((12500 - len(cells)) // 2))
    image = bytearray(0xa00)
    image[:2] = b"\x5a\xa5"
    image[0x100:0x105] = bytes((7, 0, 0, 1, 1))
    pack_into("<H", image, 0x105, 0x1111)
    pack_into(">H", image, 0x305, len(image) // 256)
    pack_into("<H", image, 0x505, len(cells))
    image.extend(cells)
    return image, expected


def check_fields(dosbox, nasm, directory):
    """Check payload, CRC status, Verify and the untouched Verify buffer."""
    for oversized in (False, True):
        image = directory / "fields.tc"
        original, expected = encode_field_fixture(oversized)
        image.write_bytes(original)
        size_code = 6 if oversized else 3
        carry_check = "jnc done" if oversized else "jc done"
        status = 0x10 if oversized else 0
        checks = f"""
    push es
    xor ax,ax
    mov es,ax
    les bx,[es:78h]
    mov byte [es:bx+3],{size_code}
    pop es
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    {carry_check}
    cmp ah,{status}
    jne done
    inc byte [stage]
    mov si,buffer
    mov di,expected
    mov cx,{len(expected)}
    cld
    repe cmpsb
    jne done
    inc byte [stage]
    mov ax,0401h
    mov cx,1
    xor dx,dx
    int 13h
    {carry_check}
    cmp ah,{status}
    jne done
    mov si,buffer
    mov di,expected
    mov cx,{len(expected)}
    repe cmpsb
    jne done
"""
        run_guest(dosbox, nasm, directory, image, checks, expected)
        assert image.read_bytes() == original
        print(f"TC {'oversized' if oversized else 'overlapping'} field: "
              "payload, CRC status and Verify passed")


def check_sector_ids(dosbox, nasm, directory):
    """Preserve eight-bit floppy IDs and ten-bit fixed-disk cylinders."""
    image = directory / "ids.tc"
    original, _ = encode_field_fixture(False)
    image.write_bytes(original)
    checks = """
    push es
    xor ax,ax
    mov es,ax
    les bx,[es:78h]
    mov byte [es:bx+3],2
    pop es
    mov ax,0201h
    mov bx,buffer
    mov cx,0bbh
    xor dx,dx
    int 13h
    jc done
    cmp ax,1
    jne done
    inc byte [stage]
    mov si,buffer
    mov cx,512
check_data:
    lodsb
    cmp al,77h
    jne done
    loop check_data
    mov ax,0401h
    mov cx,0bbh
    xor dx,dx
    int 13h
    jc done
    cmp ax,1
    jne done
    inc byte [stage]
    mov ax,0402h
    mov cx,0bbh
    xor dx,dx
    int 13h
    jnc done
    cmp ax,0401h
    jne done
    inc byte [stage]
    ; Fixed disks retain CL's high cylinder bits: C256 H1 R2.
    mov ax,0201h
    mov bx,buffer
    mov cx,0042h
    mov dx,0180h
    int 13h
    jc done
    cmp ah,0
    jne done
    mov si,buffer
    mov cx,512
check_hdd:
    lodsb
    cmp al,91h
    jne done
    loop check_hdd
"""
    hard_disk = directory / "fixed.img"
    with hard_disk.open("wb") as file:
        file.truncate(300 * 4 * 16 * 512)
        file.seek(((256 * 4 + 1) * 16 + 1) * 512)
        file.write(b"\x91" * 512)
    commands = [f'mount 2 "{hard_disk}" -t hdd -fs none -size 512,16,4,300']
    run_guest(dosbox, nasm, directory, image, checks, extra_commands=commands)
    assert image.read_bytes() == original
    print("BIOS: full-byte floppy ID, Verify partial count and fixed-disk CHS passed")


def main():
    """Run the native table regression."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--fields", action="store_true")
    modes.add_argument("--sector-ids", action="store_true")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        check = check_fields if args.fields else check_mapping
        if args.sector_ids:
            check = check_sector_ids
        check(args.dosbox.resolve(), args.nasm, Path(temp))


if __name__ == "__main__":
    main()
