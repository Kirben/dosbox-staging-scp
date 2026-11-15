#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Limit Read Track host bytes with DTL while retaining physical CRC checks."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_fm_probe import fm_image, fm_track
from run_floppy_transfer_edges_probe import DMA, one_sector
from run_floppy_write_deleted_probe import routines, setup
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


# pylint: disable-next=too-many-locals
def check_track(args, directory, extension, fm, mode):
    """Check zero, short and complete host transfers, with CRC-error controls."""
    pcjr = mode == "pcjr"
    pio = mode != "dma"
    for bad_crc in (False, True):
        image = directory / f"dtl-{fm}-{mode}-{bad_crc}.{extension}"
        original = (fm_image(extension, fm_track(0, "data" if bad_crc else ""))
                    if fm else encode_tracks(extension, [one_sector(0, bad_crc)]))
        image.write_bytes(original)
        for dtl in (0, 64, 128):
            checks = setup(pcjr) + DMA
            port = 0xf5 if pcjr else 0x3f5
            if mode == "fifo":
                checks += f"""
    mov dx,{port}
    mov al,13h
    out dx,al
    xor al,al
    out dx,al
    out dx,al
    out dx,al
    mov al,3
    out dx,al
    mov al,0dfh
    out dx,al
    mov al,3
    out dx,al
"""
            if pio:
                checks += "mov al,6\nout 0ah,al\n"
            checks += f"""
    in al,08h
    mov byte [command],{2 if fm else 0x42}
    mov byte [command+5],0
    mov byte [command+8],{dtl}
    call send_command
    call wait_result
    mov byte [stage],3
    cmp byte [ids],{0x40 if bad_crc else 0}
    jne done
    cmp byte [ids+1],{0x20 if bad_crc else 0}
    jne done
    cmp byte [ids+2],{0x20 if bad_crc else 0}
    jne done
    mov byte [stage],4
    cmp word [io_count],{dtl if pio else 0}
    jne done
    in al,08h
    test al,4
    jnz done
    xor al,al
    out 0ch,al
    in al,05h
    cmp al,{(511 - (0 if pio else dtl)) & 255}
    jne done
    in al,05h
    cmp al,{(511 - (0 if pio else dtl)) >> 8}
    jne done
    mov byte [stage],5
    mov di,buffer
    mov cx,{dtl}
    jcxz guard
    mov al,{0x11 if fm else 0x77}
    repe scasb
    jne done
guard:
    mov cx,{512 - dtl}
    mov al,0a5h
    repe scasb
    jne done
    inc byte [stage]
"""
            run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                      checks + routines(pcjr), machine="pcjr" if pcjr else "svga_s3",
                      extra_commands=("config -set cpu cpu_cycles=20000",))
        assert image.read_bytes() == original
    print(f".{extension} {'FM' if fm else 'MFM'} {mode}: "
          "DTL 0/64/128, CRCs, host counts and guards passed", flush=True)


def main():
    """Exercise every track backend through DMA and both non-DMA engines."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for extension in ("scp", "mfm", "86f", "tc"):
            for fm in (False, True):
                for mode in ("dma", "fifo", "pcjr"):
                    check_track(args, directory, extension, fm, mode)


if __name__ == "__main__":
    main()
