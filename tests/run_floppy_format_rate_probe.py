#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check selected format rates and persisted clocks across track backends."""

import argparse
from pathlib import Path
from struct import unpack
from tempfile import TemporaryDirectory

from run_floppy_read_track_size_probe import ROUTINES
from run_floppy_transfer_edges_probe import DMA, RESET, one_sector
from run_floppy_zero_sector_probe import encode_tracks
from run_formatted_floppy_probe import format_checks
from run_transcopy_layout_probe import run_guest


CLOCK_SETUP = """
    cli
    mov al,34h
    out 43h,al
    xor al,al
    out 40h,al
    out 40h,al
    sti
"""

TIMING = """
    mov dx,3f2h
    mov al,0ch
    out dx,al
    mov di,timestamps
    call stamp
    mov dx,3f2h
    mov al,1ch
    out dx,al
    mov ah,86h
    mov cx,2
    mov dx,63928
    int 15h
    mov si,read_command
    mov cx,9
    call send_command
    call wait_result
    mov di,timestamps+6
    call stamp
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    in al,08h
    test al,4
    jz done
    mov si,buffer
    mov cx,512
check_payload:
    lodsb
    cmp al,0e5h
    jne done
    loop check_payload
    mov cx,16
check_guard:
    lodsb
    cmp al,0a5h
    jne done
    loop check_guard
    mov dx,filename
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc done
    mov bx,ax
    mov dx,timestamps
    mov cx,12
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
    jmp checked
format_command db 4dh,0,2,1,54h,0e5h
read_command db 42h,0,0,0,1,2,1,54h,0ffh
filename db 'CLOCK.BIN',0
timestamps times 12 db 0
stamp:
    pushf
    cli
    push eax
    push es
    push word 40h
    pop es
    mov eax,[es:6ch]
    mov [di],eax
    xor al,al
    out 43h,al
    in al,40h
    mov ah,al
    in al,40h
    xchg al,ah
    mov [di+4],ax
    pop es
    pop eax
    popf
    ret
"""


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check_rate(dosbox, nasm, directory, extension, rate, selector, bios=False):
    """Reformat an original 250-kbps track at the selected clock."""
    image = directory / f"{'bios' if bios else 'rate'}{rate}.{extension}"
    original = encode_tracks(extension, [one_sector(2)])
    image.write_bytes(original)
    overlay = Path(str(image) + ".overlay")
    saved = b""
    for writing in (True, False):
        selected = 2 if bios and not writing else selector
        checks = CLOCK_SETUP + f"""
    mov dx,3f7h
    mov al,{selected}
    out dx,al
"""
        if writing and bios:
            checks += """
    mov ax,351eh
    int 21h
    mov byte [es:bx+8],0e5h
    push cs
    pop es
    mov dword [buffer],02010000h
    mov ax,0501h
    mov bx,buffer
    xor cx,cx
    xor dx,dx
    int 13h
    jc done
"""
        else:
            checks += format_checks(2) if writing else RESET
        checks += DMA.replace("mov cx,512", "mov cx,528") + TIMING
        run_guest(dosbox, nasm, directory, image,
                  checks + ROUTINES + "checked:\n",
                  extra_commands=("config -set cpu cpu_cycles=20000",),
                  read_only=not writing)
        t0, p0, t1, p1 = unpack("<IHIH", (directory / "CLOCK.BIN").read_bytes())
        elapsed = (65536 * (t1 - t0) + p0 - p1) * 1000 / 1193182
        # One turn from motor start to index, then ID-to-data and data+CRC.
        expected = 200 + (768 + 514 * 16) / (2 * rate)
        assert abs(elapsed - expected) < 1, (extension, rate, writing, elapsed, expected)
        if writing:
            saved = overlay.read_bytes()
            assert saved[6] == 8
            assert int.from_bytes(saved[22:24], "little") == rate
        else:
            assert overlay.read_bytes() == saved
        assert image.read_bytes() == original
        print(f".{extension} {rate} kbps {'format' if writing else 'restart'}: "
              f"{elapsed:.3f} ms, payload, DMA and guard passed", flush=True)


def main():
    """Exercise every selectable rate with a different original density."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        for extension in ("tc", "mfm", "scp", "86f"):
            for rate, selector in ((500, 0), (250, 2), (300, 1), (1000, 3)):
                check_rate(args.dosbox.resolve(), args.nasm, Path(temp),
                           extension, rate, selector)
            check_rate(args.dosbox.resolve(), args.nasm, Path(temp),
                       extension, 250, 3, bios=True)


if __name__ == "__main__":
    main()
