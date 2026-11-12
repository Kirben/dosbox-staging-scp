#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check physical field sizes, CRCs and timing after overlay formatting."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_transfer_edges_probe import DMA, RESET, one_sector, program
from run_floppy_overlap_write_probe import PARTIAL_FORMAT
from run_floppy_read_track_size_probe import ROUTINES as FAST_ROUTINES
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def format_checks(id_size):
    """Supply one ID through DMA; the caller provides command N separately."""
    return RESET + DMA.replace(
        "mov al,0ffh\n    out 05h,al", "mov al,3\n    out 05h,al").replace(
            "mov al,1\n    out 05h,al", "xor al,al\n    out 05h,al") + f"""
    mov dword [buffer],{(id_size << 24) | (1 << 16)}
    mov al,4ah
    out 0bh,al
    mov si,format_command
    mov cx,6
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    inc byte [stage]
    call read_id
    cmp byte [ids+6],{id_size}
    jne done
    inc byte [stage]
"""


def read_checks(size_code, status_1, status_2, fill):
    """Check DMA count, payload and a guard after the requested field."""
    size = 128 << size_code
    return DMA.replace("mov cx,512", "mov cx,4112").replace(
        "mov al,1\n    out 05h,al", f"mov al,{(size - 1) >> 8}\n    out 05h,al") + f"""
    mov si,read_command
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids+1],{status_1}
    jne done
    cmp byte [ids+2],{status_2}
    jne done
    inc byte [stage]
    in al,08h
    test al,4
    jz done
    mov si,buffer
    mov cx,{size}
check_payload:
    lodsb
    cmp al,{fill}
    jne done
    loop check_payload
    mov cx,16
check_guard:
    lodsb
    cmp al,0a5h
    jne done
    loop check_guard
"""


def check_sizes(dosbox, nasm, directory, extension):
    """Retain ID N1 while formatting either a 256- or 512-byte field."""
    for size_code in (1, 2):
        image = directory / f"size{size_code}.{extension}"
        original = encode_tracks(extension, [one_sector(2)])
        image.write_bytes(original)
        commands = (f"format_command db 4dh,0,{size_code},1,54h,0e5h\n"
                    f"read_command db 42h,0,0,0,1,{size_code},1,54h,0ffh\n")
        checks = read_checks(size_code, 4 if size_code == 2 else 0, 0, 0xe5)
        run_guest(dosbox, nasm, directory, image,
                  program(format_checks(1) + checks, commands), read_only=False)
        overlay = Path(str(image) + ".overlay")
        saved = overlay.read_bytes()
        # Ordinary formats retain the old layout. A mismatched command N
        # needs version 7, whose format record stores the physical size.
        assert saved[6] == (7 if size_code == 2 else 4)
        run_guest(dosbox, nasm, directory, image, program(RESET + checks, commands))
        assert overlay.read_bytes() == saved
        if size_code == 2:
            check_invalid_overlay(dosbox, nasm, directory, image, saved)
        assert image.read_bytes() == original


def check_invalid_overlay(dosbox, nasm, directory, image, saved):
    """Reject an invalid physical size and a truncated version-7 format ID."""
    overlay = Path(str(image) + ".overlay")
    invalid = bytearray(saved)
    # Header (16), track address (3), count and fill precede command N.
    invalid[21] = 8
    for damaged in (invalid, saved[:-1]):
        overlay.write_bytes(damaged)
        run_guest(dosbox, nasm, directory, image, """
    mov ax,0800h
    xor dx,dx
    int 13h
    jnc done
""")
        assert overlay.read_bytes() == damaged
    overlay.write_bytes(saved)


def check_crc(dosbox, nasm, directory, extension):
    """Check a shorter command's CRC before and after reopening a format."""
    image = directory / f"crc.{extension}"
    original = encode_tracks(extension, [one_sector(2)])
    image.write_bytes(original)
    commands = ("format_command db 4dh,0,2,1,54h,0e5h\n"
                "read_command db 42h,0,0,0,1,1,1,54h,0ffh\n")
    checks = read_checks(1, 0x24, 0x20, 0xe5)
    run_guest(dosbox, nasm, directory, image,
              program(format_checks(2) + checks, commands), read_only=False)
    run_guest(dosbox, nasm, directory, image, program(RESET + checks, commands))
    assert image.read_bytes() == original


def check_timing(dosbox, nasm, directory, extension):
    """A 512-byte transfer at 250 kbps cannot complete within 10 ms."""
    image = directory / f"timing.{extension}"
    original = encode_tracks(extension, [one_sector(2)])
    image.write_bytes(original)
    for case in ("original", "format", "reopen"):
        checks = format_checks(2) if case == "format" else RESET
        checks += DMA + """
    mov dx,3f2h
    mov al,0ch
    out dx,al
    mov al,1ch
    out dx,al
    mov ah,86h
    mov cx,2
    mov dx,63928
    int 15h
    mov si,read_command
    mov cx,9
    call send_command
    mov ah,86h
    xor cx,cx
    mov dx,10000
    int 15h
    mov dx,3f4h
    in al,dx
    cmp al,10h
    jne done
    cmp byte [buffer],0a5h
    jne done
    inc byte [stage]
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
""" + f"""
    cmp byte [buffer],{0x77 if case == 'original' else 0xe5}
    jne done
"""
        commands = ("format_command db 4dh,0,2,1,54h,0e5h\n"
                    "read_command db 42h,0,0,0,1,2,1,54h,0ffh\n")
        run_guest(dosbox, nasm, directory, image, program(checks, commands),
                  extra_commands=("config -set cpu cpu_cycles=20000",),
                  read_only=case != "format")
        assert image.read_bytes() == original


def check_partial_ids(dosbox, nasm, directory, extension):
    """Keep surviving ID-only sectors when a format stops before their IDs."""
    cells = one_sector(2)
    sync = b"\x44\x89" * 3
    mark = cells.find(sync, cells.find(sync) + len(sync))
    cells[mark:mark + len(sync)] = b"\x92\x54" * 3
    image = directory / f"idonly.{extension}"
    original = encode_tracks(extension, [cells])
    image.write_bytes(original)
    checks = """
    xor bx,bx
    mov bp,2
check_ids:
    call read_id
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+5],1
    jne check_new_id
    or bl,1
check_new_id:
    cmp byte [ids+5],9
    jne next_id
    or bl,2
next_id:
    dec bp
    jnz check_ids
    cmp bl,3
    jne done
    jmp checked
""" + FAST_ROUTINES + "checked:\n"
    for writing in (True, False):
        prefix = PARTIAL_FORMAT if writing else RESET
        run_guest(dosbox, nasm, directory, image, prefix + checks,
                  extra_commands=("config -set cpu cpu_cycles=20000",),
                  read_only=not writing)
        assert image.read_bytes() == original


def main():
    """Run each formatting regression independently across track backends."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--case", required=True, choices=("size", "crc", "timing"))
    args = parser.parse_args()
    check = {"size": check_sizes, "crc": check_crc, "timing": check_timing}[args.case]
    with TemporaryDirectory() as temp:
        for extension in ("tc", "mfm", "scp", "86f"):
            check(args.dosbox.resolve(), args.nasm, Path(temp), extension)
            if args.case == "timing":
                check_partial_ids(args.dosbox.resolve(), args.nasm, Path(temp), extension)
            print(f".{extension}: formatted {args.case} and restart passed", flush=True)


if __name__ == "__main__":
    main()
