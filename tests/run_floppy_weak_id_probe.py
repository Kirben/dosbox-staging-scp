#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode weak ID bytes and sync cells before rotational Read ID searches."""

import argparse
from pathlib import Path
from struct import unpack_from
from tempfile import TemporaryDirectory

from run_floppy_read_deleted_probe import make_track
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


CHECKS = """
    mov dx,3f2h
    xor al,al
    out dx,al
    mov al,1ch
    out dx,al
    mov dx,3f5h
    mov cx,4
reset_status:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop reset_status
    mov di,buffer
    mov bp,64
next_id:
    mov dx,3f5h
    mov al,4ah
    out dx,al
    xor al,al
    out dx,al
    mov si,600
wait_result:
    mov dx,3f4h
    in al,dx
    and al,0d0h
    cmp al,0d0h
    je result
    mov ah,86h
    xor cx,cx
    mov dx,1000
    int 15h
    dec si
    jnz wait_result
    jmp done
result:
    mov dx,3f5h
    mov cx,7
read_byte:
    in al,dx
    stosb
    loop read_byte
    dec bp
    jnz next_id
    mov dx,name
    xor cx,cx
    mov ah,3ch
    int 21h
    jc done
    mov bx,ax
    mov dx,buffer
    mov cx,448
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
    jmp captured
name db 'IDS.BIN',0
captured:
"""


def capture(kind):
    """Annotate exactly one ID data cell or one sync cell in an 86F surface."""
    cells = make_track(((1, 0xfb, "", 0x11), (2, 0xfb, "", 0x22)), 2)
    first_id = cells.find(b"\x44\x89" * 3)
    if kind == "hidden-sync":
        cells[first_id] &= ~0x40
    image = bytearray(encode_tracks("86f", [cells]))
    record = unpack_from("<I", image, 8)[0]
    surface = bytearray(len(cells))
    if kind == "id":
        # R's low data bit: an ID of 1 becomes 0, invalidating its stored CRC.
        surface[first_id + 13] = 1
        assert cells[first_id + 13] & 1
    elif kind in ("sync", "hidden-sync"):
        surface[first_id] = 0x40
        assert bool(cells[first_id] & 0x40) == (kind == "sync")
    if kind == "data-sync":
        data_sync = cells.find(b"\x44\x89" * 3, first_id + 6)
        surface[data_sync] = 0x40
    swapped = bytearray(len(surface))
    swapped[0::2], swapped[1::2] = surface[1::2], surface[0::2]
    image[record + 10 + len(cells):] = swapped
    return image


def check_data(dosbox, nasm, directory, image, marker=0x11):
    """Match sampled IDs and DAMs, never delivering another sector's payload."""
    code = "mov di,buffer\nmov cx,8192\nmov al,0a5h\nrep stosb\n"
    for index in range(16):
        code += f"""
    mov ax,0201h
    mov bx,buffer+{index * 512}
    mov cx,1
    xor dx,dx
    int 13h
    mov [statuses+{index}],ah
"""
    code += """
    mov dx,data_name
    xor cx,cx
    mov ah,3ch
    int 21h
    jc done
    mov bx,ax
    mov dx,buffer
    mov cx,8192
    mov ah,40h
    int 21h
    mov dx,statuses
    mov cx,16
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
    jmp saved
data_name db 'DATA.BIN',0
statuses times 16 db 0
saved:
"""
    run_guest(dosbox, nasm, directory, image, code)
    raw = (directory / "DATA.BIN").read_bytes()
    assert len(raw) == 8208
    statuses = raw[8192:]
    assert 0 in statuses
    for index, status in enumerate(statuses):
        assert status in (0, 4), statuses.hex()
        payload = raw[index * 512:(index + 1) * 512]
        assert payload == bytes([0xa5 if status else marker]) * 512
    return statuses


def check_write(dosbox, nasm, directory, image):
    """Persist data through a sampled ID without stabilizing the weak ID cells."""
    code = """
    mov di,buffer
    mov cx,512
    mov al,66h
    rep stosb
    mov bp,16
retry_write:
    mov ax,0301h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jnc written
    dec bp
    jnz retry_write
    jmp done
written:
"""
    run_guest(dosbox, nasm, directory, image, code, read_only=False)
    assert Path(str(image) + ".overlay").is_file()
    # Both checks run in new emulator processes and read the persistent overlay.
    check_data(dosbox, nasm, directory, image, marker=0x66)
    run_guest(dosbox, nasm, directory, image, CHECKS)
    raw = (directory / "IDS.BIN").read_bytes()
    assert {raw[i + 5] for i in range(0, len(raw), 7)} == {0, 1, 2}


def main():
    """Compare stable, weak-ID and weak-sync tracks through direct FDC reads."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for kind in ("normal", "id", "sync", "hidden-sync", "data-sync"):
            image = directory / f"{kind}.86f"
            original = capture(kind)
            image.write_bytes(original)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image, CHECKS)
            raw = (directory / "IDS.BIN").read_bytes()
            assert len(raw) == 448
            records = [raw[i:i + 7] for i in range(0, len(raw), 7)]
            ids = {record[5] for record in records}
            if kind == "id":
                assert ids == {0, 1, 2}, ids
                for record in records:
                    assert record[:3] == (b"\x40\x20\0" if record[5] == 0 else bytes(3))
            else:
                assert ids == {1, 2}, ids
                assert all(record[:3] == bytes(3) for record in records)
                repeated_second = any(a[5] == b[5] == 2 for a, b in zip(records, records[1:]))
                assert repeated_second == (kind in ("sync", "hidden-sync"))
            assert image.read_bytes() == original
            if kind in ("id", "data-sync"):
                statuses = check_data(args.dosbox.resolve(), args.nasm, directory, image)
                if kind == "data-sync":
                    assert 4 in statuses, statuses.hex()
            print(f"{kind}: rotational IDs {sorted(ids)}, ID CRC and sync visibility passed",
                  flush=True)


if __name__ == "__main__":
    main()
