#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check bounded recalibration, independent PCN and physical-head recovery."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_id_search_probe import wait_microseconds
from run_floppy_transfer_edges_probe import RESET, ROUTINES
from run_floppy_zero_sector_probe import encode_tracks, encoded_track
from run_transcopy_layout_probe import run_guest


def sense(status, pcn=None):
    """Check Sense Interrupt status and its optional cylinder result."""
    code = f"""
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,{status}
    jne done
"""
    if pcn is not None:
        code += f"""
    in al,dx
    cmp al,{pcn}
    jne done
"""
    return code


def send(name, count):
    """Issue a command through the existing IRQ-clearing guest routine."""
    return f"mov si,{name}\nmov cx,{count}\ncall send_command\n"


def physical_track(cylinder):
    """Read the physical cylinder independently of the controller PCN."""
    return f"""
    call read_id
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+3],{cylinder}
    jne done
    inc byte [stage]
"""


def recalibrate_checks(unit, pcjr):
    """Check both sides of the documented 79-pulse failure boundary."""
    code = RESET + f"""
    mov dx,3f2h
    mov al,{0x1c if unit == 0 else 0x2d}
    out dx,al
    mov dx,3f7h
    mov al,2
    out dx,al
""" + send("specify", 3)
    commands = f"specify db 3,0ffh,2\nrecalibrate db 7,{unit | 4}\n"
    commands += f"same_zero db 0fh,{unit | 4},0\n"
    for cylinder in (79, 80, 81):
        commands += f"seek_{cylinder} db 0fh,{unit | 4},{cylinder}\n"
        code += send(f"seek_{cylinder}", 3) + wait_microseconds(200000)
        code += sense(0x20 | unit, cylinder)
        code += send("recalibrate", 2) + wait_microseconds(150000)
        code += f"""
    mov dx,3f4h
    in al,dx
    cmp al,{0x80 | (1 << unit)}
    jne done
    push ds
    xor ax,ax
    mov ds,ax
    test byte [043eh],80h
    pop ds
    jnz done
""" + sense(0x80) + wait_microseconds(20000)
        code += """
    push ds
    xor ax,ax
    mov ds,ax
    test byte [043eh],80h
    pop ds
""" + ("    jnz done\n" if pcjr else "    jz done\n")
        code += sense((0x20 if cylinder == 79 else 0x70) | unit, 0)
        remaining = max(0, cylinder - 79)
        code += physical_track(remaining)
        if remaining:
            # PCN is zero even though TRACK0 is inactive: SEEK 0 must not move.
            code += send("same_zero", 3) + wait_microseconds(1000)
            code += sense(0x20 | unit, 0) + physical_track(remaining)
            code += send("recalibrate", 2) + wait_microseconds(10000)
            code += sense(0x20 | unit, 0) + physical_track(0)
    # Select the tested drive in Read ID; the other commands embed its unit.
    routines = ROUTINES.replace(
        "    xor al,al\n    out dx,al\nwait_result:",
        f"    mov al,{unit}\n    out dx,al\nwait_result:")
    if pcjr:
        routines = routines.replace("    jz done\n", "    jnz done\n")
    return code + "    jmp checked\n" + commands + routines + "checked:\n"


def main():
    """Exercise A/B on raw and captured-track backends in PC/PCjr/Tandy modes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("img", "ima", "scp", "mfm", "86f", "tc"):
            image = directory / f"recal.{extension}"
            original = bytes(360 * 1024) if extension in ("img", "ima") else encode_tracks(
                extension, [encoded_track(cylinder, False) for cylinder in range(3)])
            image.write_bytes(original)
            for machine in (None, "pcjr", "tandy"):
                for unit in (0, 1):
                    extra = () if not unit else (f'mount 1 "{image}" -t floppy -fs none -ro',)
                    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                              recalibrate_checks(unit, machine == "pcjr"),
                              extra_commands=extra, machine=machine)
                    assert image.read_bytes() == original
                    print(f".{extension} {machine or 'pc'} {'AB'[unit]}: "
                          "79/80/81 boundary, IRQ, PCN and physical recovery passed", flush=True)


if __name__ == "__main__":
    main()
