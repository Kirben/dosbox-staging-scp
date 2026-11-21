#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check selected format rates and persisted clocks across track backends."""

import argparse
from pathlib import Path
from struct import unpack
from tempfile import TemporaryDirectory

from run_floppy_read_rate_probe import command_checks
from run_floppy_read_track_size_probe import ROUTINES
from run_floppy_transfer_edges_probe import DMA, RESET, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_formatted_floppy_probe import format_checks, read_checks
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



ED_CONFIG = """
    ; Set drive A, then override globally without changing its drive bit.
    mov si,perpendicular_mode
    mov cx,4
    call send_command
    mov al,7
    call check_ed_mode
    mov dx,3f4h
    mov al,83h
    out dx,al
    call drain_reset
    mov al,4
    call check_ed_mode
    jmp ed_config_done
check_ed_mode:
    mov bl,al
    mov dx,3f5h
    mov al,0eh
    out dx,al
    mov cx,7
discard_register:
    in al,dx
    loop discard_register
    in al,dx
    cmp al,bl
    jne done
    in al,dx
    in al,dx
    ret
ed_config_done:
"""

ED_WRITE = DMA.replace("mov al,0a5h", "mov al,0a7h").replace(
    "mov al,46h", "mov al,4ah") + """
    mov si,write_command
    mov cx,9
    call send_command
    call wait_result
    cmp word [ids],0
    jne done
    cmp byte [ids+2],0
    jne done
"""

# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check_rate(
    dosbox, nasm, directory, extension, rate, selector, bios=False, *, perpendicular=False
):
    """Reformat an original 250-kbps track at the selected clock."""
    prefix = 'ed' if perpendicular else 'bios' if bios else 'rate'
    image = directory / f"{prefix}{rate}.{extension}"
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
            if writing and perpendicular:
                checks += format_checks(2).replace(RESET, RESET + ED_CONFIG)
                checks += ED_WRITE
            else:
                checks += format_checks(2) if writing else RESET
        if not writing:
            # Use a well-separated clock: a short 300-kbps ID can still
            # be recognized at 250 kbps for some gap/phase patterns.
            wrong = 0 if rate in (250, 300) else 2
            checks += command_checks(wrong, 0x4a, "wrong", rate)
            checks += command_checks(selected, 0x4a, "restored", rate)
        timing = TIMING.replace('cmp al,0e5h', 'cmp al,0a7h') if perpendicular else TIMING
        checks += DMA.replace("mov cx,512", "mov cx,528") + timing
        if perpendicular:
            checks += "perpendicular_mode db 12h,84h,12h,3\n"
            checks += "write_command db 45h,0,0,0,1,2,1,54h,0ffh\n"
        run_guest(dosbox, nasm, directory, image,
                  checks + ROUTINES + "checked:\n",
                  extra_commands=("config -set cpu cpu_cycles=20000",),
                  read_only=not writing)
        t0, p0, t1, p1 = unpack("<IHIH", (directory / "CLOCK.BIN").read_bytes())
        elapsed = (65536 * (t1 - t0) + p0 - p1) * 1000 / 1193182
        # One turn from motor start to index, then ID-to-data and data+CRC.
        # Gap4a/IAM/Gap1, ID sync/mark/CHRN/CRC, Gap2 and data sync/mark.
        expected = 200 + (146 + 60 + 514 + (19 if perpendicular else 0)) * 8 / rate
        assert abs(elapsed - expected) < 1, (extension, rate, writing, elapsed, expected)
        if writing:
            saved = overlay.read_bytes()
            assert saved[6] == (14 if perpendicular else 11)
            assert int.from_bytes(saved[22:24], "little") == rate
        else:
            assert overlay.read_bytes() == saved
        assert image.read_bytes() == original
        print(f".{extension} {rate} kbps {'format' if writing else 'restart'}: "
              f"{elapsed:.3f} ms, payload, DMA and guard passed", flush=True)


def check_raw_ed(dosbox, nasm, directory):
    """Format all 36 sectors on a raw 2.88 MB disk and reopen sector 36."""
    image = directory / "raw-ed.img"
    image.write_bytes(bytes(2880 * 1024))
    select_rate = RESET + """
    mov dx,3f7h
    mov al,3
    out dx,al
"""
    format_dma = DMA.replace("mov al,0ffh", "mov al,143").replace(
        "mov al,1\n    out 05h,al", "xor al,al\n    out 05h,al"
    ).replace("mov al,46h", "mov al,4ah")
    format_track = format_dma + """
    mov di,buffer
    mov eax,02010000h
    mov cx,36
ed_format_ids:
    stosd
    add eax,10000h
    loop ed_format_ids
    mov si,format_command
    mov cx,6
    call send_command
    call wait_result
    cmp word [ids],0
    jne done
    inc byte [stage]
    call read_id
    cmp word [ids],0
    jne done
    cmp byte [ids+6],2
    jne done
    inc byte [stage]
"""
    commands = """perpendicular_mode db 12h,84h,12h,3
format_command db 4dh,0,2,36,53h,0e5h
write_command db 45h,0,0,0,36,2,36,53h,0ffh
read_command db 46h,0,0,0,36,2,36,53h,0ffh
"""
    run_guest(dosbox, nasm, directory, image,
              program(select_rate + ED_CONFIG + format_track + ED_WRITE, commands),
              read_only=False)
    data = image.read_bytes()
    assert data[:36 * 512] == b"\xe5" * (35 * 512) + b"\xa7" * 512
    assert data[36 * 512:] == bytes(len(data) - 36 * 512)
    run_guest(dosbox, nasm, directory, image,
              program(select_rate + read_checks(2, 0, 0, 0xa7), commands))
    print("Raw 2.88 MB format, sector 36 write and restart passed", flush=True)


def main():
    """Exercise every selectable rate with a different original density."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        check_raw_ed(args.dosbox.resolve(), args.nasm, Path(temp))
        for extension in ("tc", "mfm", "scp", "86f"):
            for rate, selector in ((500, 0), (250, 2), (300, 1), (1000, 3)):
                check_rate(args.dosbox.resolve(), args.nasm, Path(temp),
                           extension, rate, selector)
            check_rate(args.dosbox.resolve(), args.nasm, Path(temp),
                       extension, 250, 3, bios=True)
            check_rate(args.dosbox.resolve(), args.nasm, Path(temp),
                       extension, 1000, 3, perpendicular=True)


if __name__ == "__main__":
    main()
