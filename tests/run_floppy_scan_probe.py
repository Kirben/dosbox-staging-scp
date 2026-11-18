#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise 82077AA Scan status and host-to-controller transfers."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_read_deleted_probe import make_track
from run_floppy_transfer_edges_probe import DMA, RESET, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def scan_program(opcode, patterns, result, *, step=1, eot=1, count=512, tc=True):
    """Send the comparison bytes via DMA and check the result and IRQ6."""
    dma = DMA.replace("mov al,46h", "mov al,4ah")
    dma = dma.replace("mov al,0ffh\n    out 05h,al", f"mov al,{(count - 1) & 255}\n    out 05h,al")
    dma = dma.replace("mov al,1\n    out 05h,al", f"mov al,{(count - 1) >> 8}\n    out 05h,al")
    setup = "\n".join(
        f"mov al,{byte}\nmov cx,{length}\nrep stosb" for byte, length in patterns
    )
    checks = RESET + dma + "\nmov di,buffer\n" + setup + """
    in al,08h
    mov si,scan_command
    mov cx,9
    call send_command
    call wait_result
    mov di,ids
    mov si,expected_result
    mov cx,7
    repe cmpsb
    jne done
    inc byte [stage]
    in al,08h
    and al,4
""" + f"cmp al,{4 if tc else 0}\njne done\n"
    commands = (f"scan_command db {opcode},0,0,0,1,2,{eot},54h,{step}\n"
                + "expected_result db " + ",".join(map(str, result)) + "\n")
    return program(checks, commands)


def check(args, directory, extension, fields, body, *, pio=False):
    """Run one command against raw or encoded media and verify no media writes."""
    image = directory / f"scan.{extension}"
    if extension in ("img", "ima"):
        raw = bytearray(360 * 1024)
        for sector, _, _, marker in fields:
            raw[(sector - 1) * 512:sector * 512] = bytes([marker]) * 512
        original = bytes(raw)
    else:
        original = encode_tracks(extension, [make_track(fields, 2)])
    image.write_bytes(original)
    run_guest(args.dosbox.resolve(), args.nasm, directory, image, body,
              machine="pcjr" if pio else None, read_only=False)
    assert image.read_bytes() == original
    assert not list(directory.glob("*.overlay"))


def pio_program():
    """Supply exactly one sector via PCjr's native non-DMA FIFO."""
    return """
    mov dx,0f2h
    mov al,80h
    out dx,al
    mov al,81h
    out dx,al
    mov dx,0f5h
    mov cx,4
drain:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop drain
    mov al,3
    out dx,al
    mov al,0dfh
    out dx,al
    mov al,3
    out dx,al
    mov si,scan
    mov cx,9
send:
    lodsb
    out dx,al
    loop send
    xor bx,bx
    mov ecx,2000000
poll:
    mov dx,0f4h
    in al,dx
    and al,0f0h
    cmp al,0d0h
    je result_ready
    cmp al,0b0h
    jne next_poll
    cmp bx,512
    jae done
    inc bx
    inc dx
    mov al,77h
    out dx,al
next_poll:
    dec ecx
    jnz poll
    jmp done
result_ready:
    cmp bx,512
    jne done
    inc byte [stage]
    mov dx,0f5h
    mov si,expected_result
    mov cx,7
read_result:
    in al,dx
    cmp al,[si]
    jne done
    inc si
    loop read_result
    jmp checked
scan db 51h,0,0,0,1,2,1,54h,1
expected_result db 0,0,8,0,0,1,2
checked:
"""


def result(st2, sector=1, st0=0, st1=0):
    """Build the seven result bytes for cylinder zero, head zero."""
    return (st0, st1, st2, 0, 0, sector, 2)


def main():
    """Check all three comparisons, early TC, stepping, marks and CRC."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    normal = ((1, 0xfb, "", 0x77),)
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        formats = ("img", "scp") if args.smoke else ("img", "ima", "scp", "mfm", "86f", "tc")
        for extension in formats:
            check(args, directory, extension, normal,
                  scan_program(0x51, ((0x77, 512),), result(8)))
        for opcode, patterns, expected in (
            (0x51, ((0x78, 512),), 4),
            (0x59, ((0x78, 512),), 0),
            (0x5d, ((0x76, 512),), 0),
            (0x59, ((0x78, 511), (0x76, 1)), 4),
            (0x5d, ((0x76, 511), (0x78, 1)), 4),
        ):
            check(args, directory, "img", normal, scan_program(opcode, patterns, result(expected)))
        check(args, directory, "scp", normal,
              scan_program(0x51, ((0x77, 128), (0x11, 384)), result(8), count=128))
        check(args, directory, "scp", normal,
              scan_program(0x51, ((0x11, 512),), result(4), count=128))
        check(args, directory, "scp", normal,
              scan_program(0x51, ((0x11, 512),), result(4), count=1024, tc=False))
        sectors = normal + ((2, 0xfb, "", 0x33), (3, 0xfb, "", 0x55))
        check(args, directory, "scp", sectors,
              scan_program(0x51, ((0x11, 512), (0x55, 512)), result(8, 3),
                           step=2, eot=3, count=1024))
        deleted = ((1, 0xf8, "", 0x77), (2, 0xfb, "", 0x33))
        check(args, directory, "scp", deleted,
              scan_program(0x51, ((0x77, 512),), result(0x40, st0=0x40), eot=2, tc=False))
        check(args, directory, "scp", deleted,
              scan_program(0x71, ((0x33, 512),), result(0x48, 2), eot=2))
        check(args, directory, "scp", ((1, 0xfb, "data", 0x77),),
              scan_program(0x51, ((0x77, 512),), result(0x20, st0=0x40, st1=0x20)))
        check(args, directory, "scp", ((1, 0xfb, "id", 0x77),),
              scan_program(0x51, ((0x77, 512),), result(0, st0=0x40, st1=0x20), tc=False))
        check(args, directory, "scp", normal, pio_program(), pio=True)
    print("Floppy Scan command regressions passed", flush=True)


if __name__ == "__main__":
    main()
