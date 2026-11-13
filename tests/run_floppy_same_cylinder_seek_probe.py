#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check unchanged-cylinder SEEK avoids SRT delays but still reports completion."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_id_search_probe import wait_microseconds
from run_floppy_transfer_edges_probe import RESET, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def sense(expected, cylinder=0):
    """Read completion status without depending on a fabricated result phase."""
    return f"""
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,{expected}
    jne done
""" + (f"""
    in al,dx
    cmp al,{cylinder}
    jne done
""" if expected == 0x20 else "")


def checks(pcjr):
    """Distinguish zero-step seeks from moving seeks and recalibration."""
    code = "mov dx,3f2h\nxor al,al\nout dx,al\n" + RESET + """
    ; A deliberately long SRT makes an invented step interval visible.
    mov si,specify
    mov cx,3
    call send_command
"""
    for stage, command, early, cylinder, delay in (
        (1, "same_zero", 0x20, 0, 0),
        (2, "move_two", 0x80, 2, 70000),
        (3, "same_two", 0x20, 2, 0),
        (4, "recalibrate", 0x80, 0, 70000),
        (5, "recalibrate", 0x80, 0, 35000),
    ):
        code += f"""
    mov byte [stage],{stage}
    mov si,{command}
    mov cx,{2 if command == 'recalibrate' else 3}
    call send_command
""" + wait_microseconds(1000) + sense(early, cylinder)
        if delay:
            code += wait_microseconds(delay) + sense(0x20, cylinder)
        code += f"""
    xor ax,ax
    mov es,ax
    test byte [es:43eh],80h
    {'jnz' if pcjr else 'jz'} done
    push cs
    pop es
"""
    return code


COMMANDS = """
specify db 3,10h,0
same_zero db 0fh,0,0
move_two db 0fh,0,2
same_two db 0fh,0,2
recalibrate db 7,0
"""


def main():
    """Check timings and completion/IRQ6 semantics across all six formats."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for extension in ("img", "ima", "scp", "mfm", "86f", "tc"):
            image = directory / f"seek.{extension}"
            original = (bytes(360 * 1024) if extension in ("img", "ima") else
                        encode_tracks(extension, [one_sector(2)] * 3))
            image.write_bytes(original)
            for machine in ("cga", "pcjr", "tandy"):
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          program(checks(machine == "pcjr"), COMMANDS), machine=machine)
                assert image.read_bytes() == original
                print(f"{extension} {machine}: zero-step SEEK, moving SEEK, recalibration, "
                      "Sense Interrupt and IRQ6 passed", flush=True)


if __name__ == "__main__":
    main()
