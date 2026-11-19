#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check physical motion, interruption and IRQ timing with CONFIGURE EIS."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_id_search_probe import wait_microseconds
from run_floppy_recalibrate_limit_probe import physical_track, send, sense
from run_floppy_transfer_edges_probe import DMA, RESET, program
from run_floppy_zero_sector_probe import encode_tracks, encoded_track
from run_transcopy_layout_probe import run_guest


def checks(raw):
    """Compare the same Read Data command with EIS clear and set."""
    size_code = 2 if raw else 1
    size = 128 << size_code
    dma = DMA.replace("mov al,1\n    out 05h,al",
                      f"mov al,{(size - 1) >> 8}\n    out 05h,al")
    body = RESET + """
    mov dx,3f7h
    mov al,2
    out dx,al
""" + send("specify", 3) + send("off", 4)
    body += dma + send("read", 9) + """
    call wait_result
    test byte [ids],40h
    jz done
    cmp byte [buffer],0a5h
    jne done
""" + physical_track(0)
    body += send("on", 4) + dma + send("read", 9)
    # SRT=13 and 250 kbit/s give three 6 ms steps, before data execution.
    body += wait_microseconds(8000) + """
    mov dx,3f4h
    in al,dx
    cmp al,11h
    jne done
    push ds
    xor ax,ax
    mov ds,ax
    test byte [043eh],80h
    pop ds
    jnz done
    inc byte [stage]
""" + wait_microseconds(16000) + """
    mov dx,3f4h
    in al,dx
    test al,1
    jnz done
    call wait_result
    cmp word [ids],0
    jne done
    cmp byte [ids+2],0
    jne done
    mov di,buffer
    mov al,71h
""" + f"mov cx,{size}\n" + """
    repe scasb
    jne done
    inc byte [stage]
""" + sense(0x80) + physical_track(3)
    body += dma + send("read_zero", 9) + wait_microseconds(8000) + """
    mov dx,3f2h
    mov al,0ch
    out dx,al
    call wait_result
    cmp word [ids],0460h
    jne done
    cmp byte [ids+3],2
    jne done
    cmp byte [buffer],0a5h
    jne done
    mov dx,3f2h
    mov al,1ch
    out dx,al
""" + sense(0x80) + physical_track(2)
    return program(body, f"""
specify db 3,0dfh,2
off db 13h,0,20h,0
on db 13h,0,60h,0
read db 46h,0,3,0,1,{size_code},1,54h,0ffh
read_zero db 46h,0,0,0,9,2,9,54h,0ffh
""")


def main():
    """Run the direct guest probe against each supported floppy backend."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    formats = ("img", "scp") if args.smoke else ("img", "ima", "scp", "mfm", "86f", "tc")
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for extension in formats:
            image = directory / f"implied.{extension}"
            raw = extension in ("img", "ima")
            if raw:
                data = bytearray(360 * 1024)
                offset = 3 * 2 * 9 * 512
                data[offset:offset + 512] = b"\x71" * 512
            else:
                data = encode_tracks(
                    extension, [encoded_track(cylinder, False) for cylinder in range(4)])
            image.write_bytes(data)
            run_guest(args.dosbox.resolve(), args.nasm, directory, image, checks(raw))
    print("Floppy implied seek regressions passed", flush=True)


if __name__ == "__main__":
    main()
