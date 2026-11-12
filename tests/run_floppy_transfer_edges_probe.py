#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check rotational completion, reset phase, FDC errors and short transfers."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack
from tempfile import TemporaryDirectory

from run_floppy_zero_sector_probe import encode_tracks, encoded_track
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import encode_field_fixture, run_guest


RESET = """
    mov dx,3f2h
    mov al,1ch
    out dx,al
    call drain_reset
"""

ROUTINES = """
drain_reset:
    mov dx,3f5h
    mov cx,4
drain_status:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop drain_status
    ret
clear_irq:
    push ds
    xor ax,ax
    mov ds,ax
    and byte [043eh],7fh
    pop ds
    ret
send_command:
    call clear_irq
    mov dx,3f5h
send_byte:
    lodsb
    out dx,al
    loop send_byte
    ret
read_id:
    call clear_irq
    mov dx,3f5h
    mov al,4ah
    out dx,al
    xor al,al
    out dx,al
wait_result:
    mov cx,50
wait_tick:
    push cx
    mov ah,86h
    xor cx,cx
    mov dx,10000
    int 15h
    mov dx,3f4h
    in al,dx
    and al,0d0h
    cmp al,0d0h
    pop cx
    je result_ready
    loop wait_tick
    jmp done
result_ready:
    push ds
    xor ax,ax
    mov ds,ax
    test byte [043eh],80h
    pop ds
    jz done
    mov dx,3f5h
    mov di,ids
    mov cx,7
read_result_byte:
    in al,dx
    stosb
    loop read_result_byte
    ret
ids times 7 db 0
"""

DMA = """
    mov di,buffer
    mov cx,512
    mov al,0a5h
    rep stosb
    mov al,6
    out 0ah,al
    xor al,al
    out 0ch,al
    xor eax,eax
    mov ax,ds
    shl eax,4
    add eax,buffer
    out 04h,al
    shr eax,8
    out 04h,al
    shr eax,8
    out 81h,al
    mov al,0ffh
    out 05h,al
    mov al,1
    out 05h,al
    mov al,46h
    out 0bh,al
    mov al,2
    out 0ah,al
"""


def one_sector(size_code, bad_crc=False):
    """Encode a C0/H0/R1 field with an optional data-CRC error."""
    cells = bytearray()
    previous = 0

    def append(data):
        nonlocal previous
        for byte in data:
            cells.extend(MFM[previous][byte])
            previous = byte & 1

    def field(data, damaged=False):
        nonlocal previous
        append(bytes(12))
        cells.extend(b"\x44\x89" * 3)
        previous = 1
        crc = crc_hqx(b"\xa1" * 3 + data, 0xffff) ^ int(damaged)
        append(data + pack(">H", crc))

    append(b"\x4e" * 80)
    field(bytes((0xfe, 0, 0, 1, size_code)))
    append(b"\x4e" * 22)
    field(b"\xfb" + b"\x77" * (128 << size_code), bad_crc)
    append(b"\x4e" * ((12500 - len(cells)) // 2))
    return cells


def program(body, commands=""):
    """Put data and subroutines beyond the successful execution path."""
    return body + "\n    jmp checked\n" + commands + ROUTINES + "checked:\n"


def check_timing(dosbox, nasm, directory, extension):
    """Finish a full overlapping field before searching for the next ID."""
    fixture, expected = encode_field_fixture(False)
    image = directory / f"timing.{extension}"
    original = encode_tracks(extension, [fixture[0xa00:]])
    image.write_bytes(original)
    checks = """
    push es
    xor ax,ax
    mov es,ax
    les bx,[es:78h]
    mov byte [es:bx+3],3
    pop es
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    cmp ax,1
    jne done
    mov si,buffer
    mov di,expected
    mov cx,1024
    repe cmpsb
    jne done
    inc byte [stage]
    call read_id
    cmp byte [ids+5],1
    jne done
"""
    run_guest(dosbox, nasm, directory, image, program(checks), expected)
    assert image.read_bytes() == original


def check_phase(dosbox, nasm, directory, extension):
    """Keep the motor running while asserting and releasing DOR reset."""
    image = directory / f"phase.{extension}"
    original = encode_tracks(extension, [encoded_track(1, False)])
    image.write_bytes(original)
    for resetting in (False, True):
        reset = """
    mov dx,3f2h
    mov al,18h
    out dx,al
    mov al,1ch
    out dx,al
    call drain_reset
""" if resetting else ""
        checks = RESET + """
    call read_id
    cmp byte [ids+5],1
    jne done
    inc byte [stage]
""" + reset + """
    call read_id
    cmp byte [ids+5],2
    jne done
"""
        run_guest(dosbox, nasm, directory, image, program(checks))
    assert image.read_bytes() == original


def check_errors(dosbox, nasm, directory, extension, wrong_cylinder):
    """Distinguish wrong/bad cylinders and retain the missing ID's size."""
    image = directory / f"errors.{extension}"
    cases = ((1, 1, 0x10), (255, 1, 0x12)) if wrong_cylinder else (
        (0, 0, 0), (0, 1, 0), (0, 3, 0))
    for cylinder, size_code, status_2 in cases:
        cells = encoded_track(cylinder, False) if cylinder else one_sector(size_code)
        original = encode_tracks(extension, [cells])
        image.write_bytes(original)
        sector = 1 if wrong_cylinder else 7
        checks = RESET + f"""
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids],40h
    jne done
    cmp byte [ids+1],4
    jne done
    cmp byte [ids+2],{status_2}
    jne done
    cmp byte [ids+3],{cylinder}
    jne done
    cmp byte [ids+6],{size_code}
    jne done
"""
        command = f"command db 46h,0,0,0,{sector},{size_code},{sector},54h,0ffh\n"
        run_guest(dosbox, nasm, directory, image, program(checks, command))
        assert image.read_bytes() == original


def check_dtl(dosbox, nasm, directory, extension):
    """Limit host data using DTL but still check the full field's CRC."""
    image = directory / f"dtl.{extension}"
    for non_dma, bad_crc in ((False, False), (False, True), (True, False), (True, True)):
        original = encode_tracks(extension, [one_sector(0, bad_crc)])
        image.write_bytes(original)
        specify = """
    mov al,6
    out 0ah,al
    mov si,specify
    mov cx,3
    call send_command
""" if non_dma else ""
        pio = """
    mov bx,buffer
    mov bp,16
    mov cx,65535
poll_pio:
    mov dx,3f4h
    in al,dx
    and al,0f0h
    cmp al,0d0h
    je pio_result
    cmp al,0f0h
    jne pio_wait
    test bp,bp
    jz done
    mov dx,3f5h
    in al,dx
    mov [bx],al
    inc bx
    dec bp
pio_wait:
    loop poll_pio
    jmp done
pio_result:
    test bp,bp
    jnz done
""" if non_dma else ""
        checks = RESET + DMA + specify + """
    mov si,command
    mov cx,9
    call send_command
""" + pio + f"""
    call wait_result
    cmp byte [ids+1],{0x20 if bad_crc else 0x80}
    jne done
    cmp byte [ids+2],{0x20 if bad_crc else 0}
    jne done
    inc byte [stage]
    xor al,al
    out 0ch,al
    in al,05h
    cmp al,{0xff if non_dma else 0xef}
    jne done
    in al,05h
    cmp al,1
    jne done
    in al,08h
    test al,4
    jnz done
    mov si,buffer
    mov cx,16
check_payload:
    lodsb
    cmp al,77h
    jne done
    loop check_payload
    mov cx,496
check_guard:
    lodsb
    cmp al,0a5h
    jne done
    loop check_guard
"""
        command = "command db 46h,0,0,0,1,0,1,54h,16\nspecify db 3,0dfh,1\n"
        run_guest(dosbox, nasm, directory, image, program(checks, command))
        assert image.read_bytes() == original


def main():
    """Exercise each correction independently across the track backends."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--case", required=True, choices=(
        "timing", "reset-phase", "wrong-cylinder", "missing-size", "dtl"))
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("tc", "mfm", "scp", "86f"):
            if args.case == "timing":
                check_timing(dosbox, args.nasm, directory, extension)
            elif args.case == "reset-phase":
                check_phase(dosbox, args.nasm, directory, extension)
            elif args.case == "dtl":
                check_dtl(dosbox, args.nasm, directory, extension)
            else:
                check_errors(dosbox, args.nasm, directory, extension,
                             args.case == "wrong-cylinder")
            print(f".{extension}: {args.case} passed, including IRQ6", flush=True)


if __name__ == "__main__":
    main()
