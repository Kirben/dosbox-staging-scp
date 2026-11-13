#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Persist deleted marks and rotational duplicate writes through DMA and PCjr PIO."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_read_deleted_probe import make_track
from run_floppy_transfer_edges_probe import DMA, program
from run_formatted_floppy_probe import format_checks
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def setup(pio):
    """Reset the controller and select native PCjr non-DMA transfers if requested."""
    reset = ("mov dx,0f2h\nmov al,80h\nout dx,al\nmov al,81h\nout dx,al\n"
             if pio else "mov dx,3f2h\nmov al,1ch\nout dx,al\n")
    return reset + "call drain_reset\n" + (
        "mov al,6\nout 0ah,al\nmov dx,0f5h\nmov al,3\nout dx,al\n"
        "mov al,0dfh\nout dx,al\nmov al,3\nout dx,al\n" if pio else "")


# Each transfer specifies the command, payload, and expected wire result.
# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def transfer(opcode, marker, pio, status_2=0, rejected=0, size_code=2):
    """Check full payload, status, DMA terminal count, IRQ6 and PIO byte count."""
    writing = opcode in (0x45, 0x49)
    size = 128 << size_code
    count = size
    track = opcode == 0x42
    status_1 = rejected or (0x80 if pio and not status_2 and not track else 0)
    status_0 = 0x40 if (status_1 or status_2) and not (track and not pio) else 0
    dma = DMA.replace("mov cx,512", f"mov cx,{size + 16}")
    dma = dma.replace("mov al,0ffh\n    out 05h,al", f"mov al,{(count-1) & 255}\n    out 05h,al")
    dma = dma.replace("mov al,1\n    out 05h,al", f"mov al,{(count-1) >> 8}\n    out 05h,al")
    checks = dma
    if writing:
        checks += f"mov di,buffer\nmov cx,{size}\nmov al,{marker}\nrep stosb\n"
        checks += "mov al,4ah\nout 0bh,al\n"
    if pio:
        checks += "mov al,6\nout 0ah,al\n"
    checks += f"""
    mov byte [command],{opcode}
    mov byte [command+5],{size_code}
    in al,08h
    push es
    xor ax,ax
    mov es,ax
    and byte [es:43eh],7fh
    pop es
    call send_command
    call wait_result
    cmp byte [ids],{status_0}
    jne done
    cmp byte [ids+1],{status_1}
    jne done
    cmp byte [ids+2],{status_2}
    jne done
    cmp byte [ids+3],{0 if pio or status_2 or rejected or track else 1}
    jne done
    cmp byte [ids+4],0
    jne done
    cmp byte [ids+5],1
    jne done
    cmp byte [ids+6],{size_code}
    jne done
    cmp word [io_count],{size if pio and not rejected else 0}
    jne done
    in al,08h
    and al,4
    cmp al,{0 if pio or rejected else 4}
    jne done
    push es
    xor ax,ax
    mov es,ax
    test byte [es:43eh],80h
    pop es
    {'jnz' if pio else 'jz'} done
    inc byte [stage]
    mov di,buffer
    mov cx,{size}
    mov al,{marker if writing or not rejected else 0xa5}
    repe scasb
    jne done
    mov cx,16
    mov al,0a5h
    repe scasb
    jne done
    inc byte [stage]
"""
    return checks


def routines(pio):
    """Poll both PIO directions independently of result-phase bytes."""
    port = 0xf4 if pio else 0x3f4
    return f"""
    jmp checked
command db 49h,0,0,0,1,2,1,54h,0ffh
ids times 7 db 0
io_count dw 0
send_command:
    mov dx,{port+1}
    mov si,command
    mov cx,9
send_byte:
    lodsb
    out dx,al
    loop send_byte
    ret
wait_result:
    mov word [io_count],0
    mov di,buffer
    mov si,buffer
    mov ecx,2000000
poll:
    mov dx,{port}
    in al,dx
    and al,0f0h
    cmp al,0d0h
    je result_ready
    cmp al,0f0h
    je read_byte
    cmp al,0b0h
    jne next_poll
    cmp si,buffer+4096
    jae done
    inc dx
    lodsb
    out dx,al
    inc word [io_count]
    jmp next_poll
read_byte:
    cmp di,buffer+4096
    jae done
    inc dx
    in al,dx
    stosb
    inc word [io_count]
next_poll:
    dec ecx
    jnz poll
    jmp done
result_ready:
    mov dx,{port+1}
    mov di,ids
    mov cx,7
result_byte:
    in al,dx
    stosb
    loop result_byte
    ret
drain_reset:
    mov dx,{port+1}
    mov cx,4
drain:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop drain
    ret
checked:
"""


# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def run(args, directory, image, pio, checks, writable=False):
    """Launch a fresh emulator so overlay replay cannot rely on memory state."""
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              setup(pio) + checks + routines(pio), read_only=not writable,
              machine="pcjr" if pio else "svga_s3",
              extra_commands=("config -set cpu cpu_cycles=20000",))


# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def check_track(args, directory, extension, pio, duplicate=False, size_code=2):
    """Write F8, replay it, restore FB, and preserve the other physical copy."""
    fields = ((1, 0xfb, "", 0x11),)
    if duplicate:
        fields += ((1, 0xfb, "", 0x77),)
    image = directory / f"disk-{pio}-{duplicate}-{size_code}.{extension}"
    # Keep the first duplicate after the explicit four-millisecond head load.
    track = make_track(fields, size_code, lead_gap=256 if pio else 80)
    original = encode_tracks(extension, [track])
    image.write_bytes(original)
    # A normal write first creates an old, mark-free overlay to upgrade.
    run(args, directory, image, pio, transfer(0x45, 0x55, pio, size_code=size_code), True)
    run(args, directory, image, pio, transfer(0x49, 0x66, pio, size_code=size_code), True)
    overlay = Path(str(image) + ".overlay")
    assert overlay.read_bytes()[6] == 9
    if duplicate:
        checks = transfer(0x46, 0x66, pio, 0x40) + transfer(0x46, 0x77, pio)
        checks += transfer(0x4c, 0x66, pio)
    else:
        checks = transfer(0x4c, 0x66, pio, size_code=size_code)
        checks += transfer(0x46, 0x66, pio, 0x40, size_code=size_code)
    # Read Track samples the rewritten mark and physical CRC bytes.
    checks += transfer(0x42, 0x66, pio, 0x40, size_code=size_code)
    saved = overlay.read_bytes()
    run(args, directory, image, pio, checks)
    assert overlay.read_bytes() == saved
    run(args, directory, image, pio, transfer(0x45, 0x88, pio, size_code=size_code), True)
    checks = transfer(0x46, 0x88, pio, size_code=size_code)
    if duplicate:
        checks += transfer(0x46, 0x77, pio)
    checks += transfer(0x4c, 0x88, pio, 0x40, size_code=size_code)
    checks += transfer(0x42, 0x88, pio, size_code=size_code)
    run(args, directory, image, pio, checks)
    assert image.read_bytes() == original
    print(f"{extension} {'PIO' if pio else 'DMA'} duplicate={duplicate} N={size_code}: "
          "F8/FB replay, CRC/status, guards and unchanged source passed", flush=True)


def check_formatted(args, directory, extension):
    """Retain format metadata in v9, and reject damaged mark/payload records."""
    image = directory / f"formatted.{extension}"
    original = encode_tracks(extension, [make_track(((1, 0xfb, "", 0x11),), 2)])
    image.write_bytes(original)
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              program(format_checks(2), "format_command db 4dh,0,2,1,54h,0e5h\n"),
              read_only=False)
    run(args, directory, image, False, transfer(0x49, 0x66, False), True)
    overlay = Path(str(image) + ".overlay")
    saved = overlay.read_bytes()
    assert saved[6] == 9 and saved[-513] == 1
    run(args, directory, image, False,
        transfer(0x4c, 0x66, False) + transfer(0x42, 0x66, False, 0x40))
    assert overlay.read_bytes() == saved
    invalid = bytearray(saved)
    invalid[-513] = 2
    for damaged in (invalid, saved[:-513], saved[:-1]):
        overlay.write_bytes(damaged)
        run_guest(args.dosbox.resolve(), args.nasm, directory, image, """
    mov ax,0800h
    xor dx,dx
    int 13h
    jnc done
""")
        assert overlay.read_bytes() == damaged
    overlay.write_bytes(saved)
    assert image.read_bytes() == original
    print(f"{extension}: format history, deleted-mark replay and malformed v9 rejection passed",
          flush=True)


def main():
    """Run all track overlays and normal-only raw controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for extension in ("scp", "mfm", "86f", "tc"):
            for pio in (False, True):
                for duplicate in (False, True):
                    check_track(args, directory, extension, pio, duplicate)
                for size_code in (1, 3):
                    check_track(args, directory, extension, pio, size_code=size_code)
            image = directory / f"protected.{extension}"
            original = encode_tracks(extension, [make_track(((1, 0xfb, "", 0x11),), 2)])
            image.write_bytes(original)
            for pio in (False, True):
                run(args, directory, image, pio, transfer(0x49, 0x66, pio, rejected=2))
            assert not Path(str(image) + ".overlay").exists()
            assert image.read_bytes() == original
            check_formatted(args, directory, extension)
        for extension in ("img", "ima"):
            image = directory / f"raw.{extension}"
            original = b"\x11" * 512 + bytes(360 * 1024 - 512)
            image.write_bytes(original)
            for pio in (False, True):
                run(args, directory, image, pio, transfer(0x49, 0x66, pio, rejected=4), True)
            assert image.read_bytes() == original
            assert not Path(str(image) + ".overlay").exists()
            print(f"{extension}: unrepresentable deleted mark rejected without writing", flush=True)


if __name__ == "__main__":
    main()
