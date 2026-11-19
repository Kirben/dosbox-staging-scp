#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check Configure polling and reset Sense Interrupt results."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_id_search_probe import wait_microseconds
from run_floppy_recalibrate_limit_probe import physical_track, send, sense
from run_floppy_transfer_edges_probe import program
from run_transcopy_layout_probe import run_guest


def check_irq(expected):
    """Check the BIOS IRQ6 completion flag without acknowledging the FDC."""
    return f"""
    push ds
    xor ax,ax
    mov ds,ax
    test byte [043eh],80h
    pop ds
    {'jz' if expected else 'jnz'} done
"""


def check_reset_status():
    """Verify one reset interrupt and four ordered logical drive statuses."""
    body = check_irq(True) + "call clear_irq\n"
    for unit in range(4):
        body += sense(0xc0 | unit, 0)
    return body + sense(0x80) + wait_microseconds(5000) + check_irq(False)


def checks(dsr):
    """Run default, early-disable, late-disable and re-enable scenarios."""
    dor = """
    mov dx,3f2h
    mov al,18h
    out dx,al
    mov al,1ch
    out dx,al
"""
    reset = """call clear_irq
    mov dx,3f4h
    mov al,83h
    out dx,al
""" if dsr else "call clear_irq\n" + dor
    # Start the motor and drain any initial reset before the measured resets.
    body = dor + "call drain_reset\n" + """
    mov dx,3f7h
    mov al,3
    out dx,al
"""
    body += reset + check_irq(False) + wait_microseconds(5000) + check_reset_status()
    body += "inc byte [stage]\n" + reset + """
    mov dx,3f5h
    mov al,13h
    out dx,al
""" + wait_microseconds(5000) + check_irq(False) + """
    mov dx,3f5h
    xor al,al
    out dx,al
    mov al,30h
    out dx,al
    xor al,al
    out dx,al
""" + wait_microseconds(5000) + check_irq(False) + sense(0x80)
    # Data-command IRQ6 still works with polling disabled.
    body += "inc byte [stage]\n" + """
    mov dx,3f7h
    mov al,2
    out dx,al
""" + physical_track(0) + sense(0x80)
    body += send("enable", 4) + wait_microseconds(5000) + check_reset_status()
    # A late Configure must leave the already queued reset status readable.
    body += "inc byte [stage]\n" + reset + wait_microseconds(5000) + check_irq(True)
    body += """
    mov dx,3f5h
    mov al,13h
    out dx,al
    xor al,al
    out dx,al
    mov al,30h
    out dx,al
    xor al,al
    out dx,al
""" + check_reset_status()
    # POLL is re-enabled by every reset, even when it was previously set.
    body += "inc byte [stage]\n" + reset + wait_microseconds(5000) + check_reset_status()
    return program(body, "enable db 13h,0,20h,0\n")


def main():
    """Test both PC software reset registers with a direct guest program."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for dsr in (False, True):
            image = directory / f"poll-{'dsr' if dsr else 'dor'}.img"
            image.write_bytes(bytes(360 * 1024))
            run_guest(args.dosbox.resolve(), args.nasm, directory, image, checks(dsr))
    print("Floppy reset-poll regressions passed", flush=True)


if __name__ == "__main__":
    main()
