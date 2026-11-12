#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check physical head selection, index search limits and missing ID marks."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack, pack_into
from tempfile import TemporaryDirectory

from run_floppy_transfer_edges_probe import DMA, RESET, program
from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest


def head_track(head):
    """Encode a valid sector whose ID head need not match its physical side."""
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
    field(bytes((0xfe, 0, head, 1, 2)))
    append(b"\x4e" * 22)
    field(b"\xfb" + b"\x77" * 512)
    append(b"\x4e" * ((12500 - len(cells)) // 2))
    return cells


def scp_turns(tracks):
    """Wrap successive recorded turns of cylinder zero in an SCP image."""
    fluxes = []
    for cells in tracks:
        bits = "".join(f"{byte:08b}" for byte in cells)
        edges = [i + 1 for i, value in enumerate(bits) if value == "1"]
        flux = bytearray()
        last = 0
        for edge in edges:
            flux.extend(pack(">H", (edge - last) * 80))
            last = edge
        fluxes.append((len(bits) * 80, flux))
    image = bytearray(0x2b0)
    image[:9] = b"SCP" + bytes((0x10, 0x31, len(tracks), 0, 0, 1))
    pack_into("<I", image, 16, len(image))
    image.extend(b"TRK\0")
    offset = 4 + len(tracks) * 12
    for duration, flux in fluxes:
        image.extend(pack("<III", duration, len(flux) // 2, offset))
        offset += len(flux)
    for _, flux in fluxes:
        image.extend(flux)
    pack_into("<I", image, 12, sum(image[16:]) & 0xffffffff)
    return image


def check_heads(dosbox, nasm, directory, extension):
    """Read and verify nonstandard ID heads without changing physical sides."""
    for head in (0, 1):
        image = directory / f"head{head}.{extension}"
        original = encode_tracks(extension, [head_track(head)])
        image.write_bytes(original)
        checks = RESET + DMA + f"""
    call read_id
    cmp byte [ids+4],{head}
    jne done
    inc byte [stage]
"""
        for opcode in (0x46, 0x42, 0x56):
            checks += DMA + f"""
    mov si,command_{opcode}
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    cmp byte [ids+4],{head}
    jne done
    cmp byte [buffer],{0xa5 if opcode == 0x56 else 0x77}
    jne done
    inc byte [stage]
"""
        commands = "".join(
            f"command_{opcode} db {opcode},0,0,{head},1,2,1,54h,0ffh\n"
            for opcode in (0x46, 0x42, 0x56))
        run_guest(dosbox, nasm, directory, image, program(checks, commands))
        assert image.read_bytes() == original
    check_head_write(dosbox, nasm, directory, image, original)


def check_head_write(dosbox, nasm, directory, image, original):
    """Write the H1 ID on physical side zero and remount its overlay."""
    for writing in (True, False):
        checks = RESET + DMA
        if writing:
            checks += """
    mov di,buffer
    mov cx,512
    mov al,66h
    rep stosb
    mov al,4ah
    out 0bh,al
"""
        checks += """
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    mov si,buffer
    mov cx,512
check_payload:
    lodsb
    cmp al,66h
    jne done
    loop check_payload
"""
        opcode = 0x45 if writing else 0x46
        commands = f"command db {opcode},0,0,1,1,2,1,54h,0ffh\n"
        run_guest(dosbox, nasm, directory, image, program(checks, commands),
                  read_only=not writing)
        assert image.read_bytes() == original
    assert Path(str(image) + ".overlay").is_file()


def wait_microseconds(duration):
    """Wait in guest time without consuming a pending FDC result."""
    return f"""
    mov ah,86h
    mov cx,{duration >> 16}
    mov dx,{duration & 0xffff}
    int 15h
"""


def check_indexes(dosbox, nasm, directory):
    """Fail after two physical indexes, including targets on later turns."""
    cells = head_track(0)
    blank = b"\x92\x54" * 6250
    for turns, late_target in ((1, False), (5, False), (5, True)):
        tracks = [blank] * 4 + [cells] if late_target else [cells] * turns
        image = directory / "indexes.scp"
        original = scp_turns(tracks)
        image.write_bytes(original)
        checks = RESET + DMA + """
    mov si,command
    mov cx,9
    call send_command
""" + wait_microseconds(250000) + """
    mov dx,3f4h
    in al,dx
    cmp al,10h
    jne done
    inc byte [stage]
""" + wait_microseconds(200000) + f"""
    mov dx,3f4h
    in al,dx
    cmp al,0d0h
    jne done
    inc byte [stage]
    call wait_result
    cmp byte [ids+1],{1 if late_target else 4}
    jne done
    cmp byte [ids+2],0
    jne done
    cmp byte [buffer],0a5h
    jne done
"""
        sector = 1 if late_target else 7
        commands = f"command db 46h,0,0,0,{sector},2,{sector},54h,0ffh\n"
        run_guest(dosbox, nasm, directory, image, program(checks, commands))
        assert image.read_bytes() == original


def check_blank(dosbox, nasm, directory, extension):
    """Distinguish absent ID marks from an unmatched sector on a valid track."""
    image = directory / f"blank.{extension}"
    original = encode_tracks(extension, [head_track(0), b"\x92\x54" * 6250])
    image.write_bytes(original)
    checks = RESET + DMA + """
    mov si,seek
    mov cx,3
    call send_command
""" + wait_microseconds(10000) + """
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    cmp al,1
    jne done
    inc byte [stage]
    call read_id
    cmp byte [ids+1],1
    jne done
    cmp byte [ids+2],0
    jne done
    cmp byte [buffer],0a5h
    jne done
"""
    run_guest(dosbox, nasm, directory, image, program(checks, "seek db 0fh,0,1\n"))
    assert image.read_bytes() == original


def main():
    """Run each correction independently, with unchanged read-only fixtures."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--case", required=True, choices=("head", "index", "blank"))
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        if args.case == "index":
            check_indexes(dosbox, args.nasm, directory)
        else:
            for extension in ("tc", "mfm", "scp", "86f"):
                check = check_heads if args.case == "head" else check_blank
                check(dosbox, args.nasm, directory, extension)
        print(f"Floppy {args.case}: status, IRQ6 and unchanged images passed", flush=True)


if __name__ == "__main__":
    main()
