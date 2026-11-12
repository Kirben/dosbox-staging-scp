#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check command-sized Read Track CRCs and physical completion timing."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_transfer_edges_probe import DMA, RESET, one_sector
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import encode_field_fixture, run_guest


# Poll without BIOS sleeps: a 10 ms polling interval can hide premature
# completion by advancing past the embedded ID before Read ID is issued.
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
send_command:
    mov dx,3f5h
send_byte:
    lodsb
    out dx,al
    loop send_byte
    ret
read_id:
    mov dx,3f5h
    mov al,4ah
    out dx,al
    xor al,al
    out dx,al
wait_result:
    mov ecx,1000000
poll_result:
    mov dx,3f4h
    in al,dx
    and al,0d0h
    cmp al,0d0h
    je result_ready
    dec ecx
    jnz poll_result
    jmp done
result_ready:
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


def check_read(dosbox, nasm, directory, extension, case):
    """Check CRC, payload, DMA bounds and the ID following a completed read."""
    fixture, payload = encode_field_fixture(False)
    if case == "crc":
        cells = one_sector(3)
        size_code, status_1, status_2 = 2, 0x24, 0x20
        payload = b"\x77" * 512
    else:
        cells = fixture[0xa00:]
        size_code = 3 if case == "timing" else 4
        status_1, status_2 = (0, 0) if case == "timing" else (0x24, 0x20)
    size = 128 << size_code
    image = directory / f"{case}.{extension}"
    original = encode_tracks(extension, [cells])
    image.write_bytes(original)
    checks = RESET + DMA.replace("mov cx,512", "mov cx,4112").replace(
        "mov al,1\n    out 05h,al", f"mov al,{(size - 1) >> 8}\n    out 05h,al")
    checks += f"""
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids+1],{status_1}
    jne done
    cmp byte [ids+2],{status_2}
    jne done
    inc byte [stage]
"""
    if case != "crc":
        checks += """
    call read_id
    cmp byte [ids+5],1
    jne done
    inc byte [stage]
"""
    checks += f"""
    in al,08h
    test al,4
    jz done
    xor al,al
    out 0ch,al
    in al,05h
    cmp al,0ffh
    jne done
    in al,05h
    cmp al,0ffh
    jne done
    mov si,buffer
    mov di,expected
    mov cx,{len(payload)}
    repe cmpsb
    jne done
    mov si,buffer+{size}
    mov cx,16
check_guard:
    lodsb
    cmp al,0a5h
    jne done
    loop check_guard
    jmp checked
command db 42h,0,0,0,1,{size_code},1,54h,0ffh
""" + ROUTINES + "checked:\n"
    run_guest(dosbox, nasm, directory, image, checks, payload)
    assert image.read_bytes() == original


def main():
    """Exercise short, equal-size and long transfers on each track backend."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--case", required=True, choices=("crc", "timing"))
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("tc", "mfm", "scp", "86f"):
            check_read(args.dosbox.resolve(), args.nasm, directory, extension,
                       args.case)
            if args.case == "timing":
                check_read(args.dosbox.resolve(), args.nasm, directory,
                           extension, "long-control")
            print(f".{extension}: Read Track {args.case} passed", flush=True)


if __name__ == "__main__":
    main()
