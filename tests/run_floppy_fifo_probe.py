#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check progressive DMA, FIFO service latency, terminal count and persistence."""

import argparse
from pathlib import Path
from struct import unpack
from tempfile import TemporaryDirectory

from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS
from run_floppy_transfer_edges_probe import DMA, RESET, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


PROGRESS = """
    mov di,timestamps
    call stamp
    mov si,transfer
    mov cx,9
    call send_command
    mov ecx,1000000
poll_progress:
    call dma_count
    cmp ax,511
    jne progressed
    dec ecx
    jnz poll_progress
    jmp done
progressed:
    cmp ax,480
    jb done
    cmp ax,510
    ja done
    mov [first_count],ax
    mov di,timestamps+6
    call stamp
    inc byte [stage]
"""
SAVE = """
    mov dx,filename
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc done
    mov bx,ax
    mov dx,timestamps
    mov cx,14
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
"""
COUNT = """
dma_count:
    xor al,al
    out 0ch,al
    in al,05h
    mov ah,al
    in al,05h
    xchg al,ah
    ret
"""


def fixture(extension):
    """Make one ordinary sector on a raw disk or an encoded track."""
    if extension in ("img", "ima"):
        data = bytearray(360 * 1024)
        data[:512] = b"\x77" * 512
        return data
    return encode_tracks(extension, [one_sector(2)])


def setup(configuration):
    """Reset to one-byte compatibility or enable a selected FIFO threshold."""
    code = CLOCK_SETUP + RESET
    commands = DECLARATIONS.replace(
        "timestamps times 12 db 0", "timestamps times 12 db 0\nfirst_count dw 0")
    if configuration is not None:
        code += "mov si,configure\nmov cx,4\ncall send_command\n"
        value = 15 if configuration == "reset" else configuration
        commands += f"configure db 13h,0,{value},0\n"
        if configuration == "reset":
            code += "mov dx,3f2h\nxor al,al\nout dx,al\n" + RESET
    return code, commands


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check_read(dosbox, nasm, directory, extension, configuration, masked, opcode=0x46):
    """Observe early DMA progress and distinguish FIFO recovery from overrun."""
    image = directory / f"read-{opcode}-{configuration}-{masked}.{extension}"
    original = fixture(extension)
    image.write_bytes(original)
    checks, commands = setup(configuration)
    checks += DMA.replace("mov cx,512", "mov cx,528") + """
    mov dx,3f2h
    mov al,0ch
    out dx,al
    mov al,1ch
    out dx,al
""" + PROGRESS
    if masked:
        checks += """
    mov al,6
    out 0ah,al
    call dma_count
    mov [first_count],ax
    call delay_200us
    mov al,2
    out 0ah,al
"""
    success = not masked or (isinstance(configuration, int) and not configuration & 0x20)
    checks += f"""
    call wait_result
    cmp byte [ids+1],{0 if success else 0x10}
    jne done
    cmp byte [ids+2],0
    jne done
    in al,08h
    and al,4
    cmp al,{4 if success else 0}
    jne done
    mov si,buffer
    mov cx,512
"""
    if success:
        checks += """
read_payload:
    lodsb
    cmp al,77h
    jne done
    loop read_payload
"""
    else:
        checks += """
    mov cx,511
    sub cx,[first_count]
read_prefix:
    lodsb
    cmp al,77h
    jne done
    loop read_prefix
    mov cx,[first_count]
    inc cx
read_untouched:
    lodsb
    cmp al,0a5h
    jne done
    loop read_untouched
"""
    checks += """
    mov cx,16
read_guard:
    lodsb
    cmp al,0a5h
    jne done
    loop read_guard
""" + SAVE
    commands += f"transfer db {opcode},0,0,0,1,2,1,54h,0ffh\n" + COUNT + DELAY
    run_guest(dosbox, nasm, directory, image, program(checks, commands),
              extra_commands=("config -set cpu cpu_cycles=20000",))
    t0, p0, t1, p1, count = unpack("<IHIHH", (directory / "CLOCK.BIN").read_bytes())
    elapsed = (65536 * (t1 - t0) + p0 - p1) * 1000 / 1193182
    assert 480 <= count <= 510, count
    assert image.read_bytes() == original
    print(f".{extension} FIFO={configuration} mask={masked}: "
          f"first DMA after {elapsed:.3f} ms; status, TC, IRQ6 and guards passed", flush=True)
    return elapsed


DELAY = """
pit_count:
    xor al,al
    out 43h,al
    in al,40h
    mov ah,al
    in al,40h
    xchg al,ah
    ret
delay_200us:
    call pit_count
    mov bx,ax
wait_200us:
    call pit_count
    mov dx,bx
    sub dx,ax
    cmp dx,239
    jb wait_200us
    ret
"""


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check_write(dosbox, nasm, directory, extension, early_tc):
    """Persist exactly the DMA-supplied prefix and zero-filled remainder."""
    image = directory / f"write-{early_tc}.{extension}"
    original = fixture(extension)
    image.write_bytes(original)
    checks, commands = setup(15)
    dma = DMA.replace("mov al,0a5h", "mov al,77h").replace("mov al,46h", "mov al,4ah")
    if early_tc:
        dma = dma.replace("mov al,0ffh\n    out 05h,al", "mov al,63\n    out 05h,al")
        dma = dma.replace("mov al,1\n    out 05h,al", "xor al,al\n    out 05h,al")
        checks += dma + "mov si,transfer\nmov cx,9\ncall send_command\n" + """
    mov ecx,1000000
wait_early_tc:
    call dma_count
    cmp ax,0ffffh
    je early_tc
    dec ecx
    jnz wait_early_tc
    jmp done
early_tc:
    in al,08h
    test al,4
    jz done
    mov dx,3f4h
    in al,dx
    and al,0d0h
    cmp al,10h
    jne done
    push ds
    xor ax,ax
    mov ds,ax
    test byte [043eh],80h
    pop ds
    jnz done
"""
    else:
        checks += dma + PROGRESS + """
    mov al,6
    out 0ah,al
    call dma_count
    mov [first_count],ax
"""
    checks += f"""
    call wait_result
    cmp byte [ids+1],{0 if early_tc else 0x10}
    jne done
"""
    if not early_tc:
        checks += "in al,08h\ntest al,4\njnz done\n"
    checks += SAVE
    commands += "transfer db 45h,0,0,0,1,2,1,54h,0ffh\n" + COUNT
    run_guest(dosbox, nasm, directory, image, program(checks, commands),
              extra_commands=("config -set cpu cpu_cycles=20000",), read_only=False)
    count = unpack("<IHIHH", (directory / "CLOCK.BIN").read_bytes())[-1]
    prefix = 64 if early_tc else 511 - count
    expected = b"\x77" * prefix + bytes(512 - prefix)
    checks = """
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    mov si,buffer
    mov di,expected
    mov cx,512
    repe cmpsb
    jne done
"""
    run_guest(dosbox, nasm, directory, image, checks, expected)
    if extension in ("img", "ima"):
        assert image.read_bytes()[:512] == expected
    else:
        assert image.read_bytes() == original
        assert Path(str(image) + ".overlay").is_file()
    print(f".{extension} {'TC' if early_tc else 'underrun'}: "
          f"{prefix} supplied bytes, zero tail and restart persistence passed", flush=True)



# pylint: disable-next=too-many-locals
def check_pio(dosbox, nasm, directory, extension, writing):
    """Use the enhanced FIFO through RQM without any DMA transfer."""
    image = directory / f"pio-{writing}.{extension}"
    original = fixture(extension)
    image.write_bytes(original)
    checks, commands = setup(15)
    checks += "mov si,specify\nmov cx,3\ncall send_command\n"
    checks += "mov si,transfer\nmov cx,9\ncall send_command\ncall ready\n"
    if writing:
        checks += """
    mov cx,16
prefill:
    mov dx,3f5h
    mov al,55h
    out dx,al
    loop prefill
    mov dx,3f4h
    in al,dx
    test al,80h
    jnz done
    mov bp,496
"""
    else:
        checks += "mov bp,512\nmov di,buffer\n"
    checks += "call delay_200us\nnext_byte:\ncall ready\nmov dx,3f5h\n"
    checks += "mov al,55h\nout dx,al\n" if writing else "in al,dx\nstosb\n"
    checks += "dec bp\njnz next_byte\ncall wait_result\ncmp byte [ids+1],80h\njne done\n"
    if not writing:
        checks += """
    mov si,buffer
    mov cx,512
pio_payload:
    lodsb
    cmp al,77h
    jne done
    loop pio_payload
"""
    opcode = 0x45 if writing else 0x46
    commands += f"specify db 3,0dfh,3\ntransfer db {opcode},0,0,0,1,2,1,54h,0ffh\n"
    commands += DELAY + f"""
ready:
    mov ecx,1000000
ready_poll:
    mov dx,3f4h
    in al,dx
    and al,0f0h
    cmp al,{0xb0 if writing else 0xf0}
    je ready_return
    dec ecx
    jnz ready_poll
    jmp done
ready_return:
    ret
"""
    run_guest(dosbox, nasm, directory, image, program(checks, commands),
              extra_commands=("config -set cpu cpu_cycles=20000",), read_only=not writing)
    if writing:
        checks = """
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    mov si,buffer
    mov di,expected
    mov cx,512
    repe cmpsb
    jne done
"""
        run_guest(dosbox, nasm, directory, image, checks, b"\x55" * 512)
    if extension not in ("img", "ima") or not writing:
        assert image.read_bytes() == original
    print(f".{extension} FIFO PIO {'write/restart' if writing else 'read'}: "
          "RQM bursts, 200-us host delay and payload passed", flush=True)


def check_format(dosbox, nasm, directory, extension):
    """Fetch format IDs progressively, finish at index and verify after restart."""
    image = directory / f"format.{extension}"
    original = fixture(extension)
    image.write_bytes(original)
    checks, commands = setup(15)
    checks += DMA.replace("mov al,46h", "mov al,4ah").replace(
        "mov al,0ffh\n    out 05h,al", "mov al,35\n    out 05h,al").replace(
        "mov al,1\n    out 05h,al", "xor al,al\n    out 05h,al")
    checks += """
    mov bx,buffer
    mov cx,9
format_ids:
    mov word [bx],0
    mov ax,10
    sub ax,cx
    mov ah,2
    mov [bx+2],ax
    add bx,4
    loop format_ids
    mov si,format_command
    mov cx,6
    call send_command
    mov ecx,1000000
format_progress:
    call dma_count
    cmp ax,35
    jne format_started
    dec ecx
    jnz format_progress
    jmp done
format_started:
    cmp ax,20
    jb done
    cmp ax,34
    ja done
    mov ecx,1000000
format_tc:
    call dma_count
    cmp ax,0ffffh
    je format_still_busy
    dec ecx
    jnz format_tc
    jmp done
format_still_busy:
    mov dx,3f4h
    in al,dx
    and al,0d0h
    cmp al,10h
    jne done
    call wait_result
    cmp byte [ids+1],0
    jne done
    in al,08h
    test al,4
    jz done
"""
    commands = commands.replace("format_command db 4dh,0,2,1,54h,0e5h",
                                "format_command db 4dh,0,2,9,54h,0e5h")
    run_guest(dosbox, nasm, directory, image, program(checks, commands + COUNT),
              extra_commands=("config -set cpu cpu_cycles=20000",), read_only=False)
    checks = """
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    mov si,buffer
    mov di,expected
    mov cx,512
    repe cmpsb
    jne done
"""
    run_guest(dosbox, nasm, directory, image, checks, b"\xe5" * 512)
    if extension not in ("img", "ima"):
        assert image.read_bytes() == original
    print(f".{extension} format: progressive ID DMA, TC before result and persistence passed",
          flush=True)

def main():
    """Exercise progressive reads and interrupted writes on all six formats."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("img", "ima", "scp", "mfm", "86f", "tc"):
            first = check_read(args.dosbox.resolve(), args.nasm, directory, extension, 15, False)
            last = check_read(args.dosbox.resolve(), args.nasm, directory, extension, 0, False)
            assert abs(last - first - 0.480) < 0.04, (extension, first, last)
            check_read(args.dosbox.resolve(), args.nasm, directory, extension, None, True)
            check_read(args.dosbox.resolve(), args.nasm, directory, extension, 15, True)
            check_read(args.dosbox.resolve(), args.nasm, directory, extension, 0x2f, True)
            check_read(args.dosbox.resolve(), args.nasm, directory, extension, "reset", True)
            for early_tc in (False, True):
                check_write(args.dosbox.resolve(), args.nasm, directory, extension, early_tc)
            if extension not in ("img", "ima"):
                check_read(args.dosbox.resolve(), args.nasm, directory, extension, 15, False,
                           opcode=0x42)
            check_format(args.dosbox.resolve(), args.nasm, directory, extension)
            for writing in (False, True):
                check_pio(args.dosbox.resolve(), args.nasm, directory, extension, writing)


if __name__ == "__main__":
    main()
