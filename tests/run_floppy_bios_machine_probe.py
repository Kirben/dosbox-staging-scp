#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check PCjr FIFO and Tandy DMA BIOS transfers through the shared FDC."""

import argparse
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_read_deleted_probe import make_track
from run_floppy_zero_sector_probe import encode_tracks
from run_floppy_write_deleted_probe import transfer as direct_transfer, routines as direct_routines
from run_transcopy_layout_probe import run_guest


@dataclass(frozen=True)
# Wire request and expected BIOS/controller results belong to one case.
# pylint: disable-next=too-many-instance-attributes
class Case:
    """A BIOS request and the expected partial transfer."""
    opcode: int
    sector: int = 1
    count: int = 1
    status: int = 0
    completed: int = 1
    markers: tuple = (0x11,)
    st1: int = 0
    st2: int = 0


SETUP = """
    xor ax,ax
    mov es,ax
    mov ax,[es:38h]
    mov [keyboard_vector],ax
    mov ax,[es:3ah]
    mov [keyboard_vector+2],ax
    mov word [es:78h],parameters
    mov [es:7ah],cs
    in al,21h
    mov [pic_mask],al
    or al,40h
    out 21h,al
    in al,21h
    mov [expected_mask],al
"""
FINISH = """
    in al,21h
    cmp al,[expected_mask]
    jne done
    inc byte [stage]
    mov al,[pic_mask]
    out 21h,al
    xor ax,ax
    mov es,ax
    mov ax,[es:38h]
    cmp ax,[keyboard_vector]
    jne done
    inc byte [stage]
    mov ax,[es:3ah]
    cmp ax,[keyboard_vector+2]
    jne done
    inc byte [stage]
    ; Keyboard BIOS remains callable after floppy commands.
    mov ah,1
    int 16h
    jmp checked
keyboard_vector dd 0
pic_mask db 0
expected_mask db 0
parameters db 0dfh,2,25h,2,9,2ah,0ffh,50h,0f6h,15,8
checked:
"""


# pylint: disable-next=too-many-locals
def request(case, pcjr, size_code=2, offset=0x4000):
    """Check guest data, hardware results, IRQ6, DMA state and guards."""
    size = 128 << size_code
    boundary = case.status == 9
    hardware = not boundary
    st1 = 0x80 if pcjr and case.status == 0 and case.opcode != 4 else case.st1
    data_blocks = len(case.markers) if case.opcode == 2 else 0
    tc = not pcjr and not boundary and case.opcode != 4 and (
        data_blocks == case.count or (case.opcode == 3 and not case.status))
    dma_count = (0x1357 if pcjr or boundary or case.opcode == 4 else
                 0xffff if tc else case.count * size - 1)
    assembly = f"""
    mov byte [parameters+3],{size_code}
    mov ax,6000h
    mov es,ax
    mov di,{offset}
    mov cx,2048
    mov al,0a5h
    rep stosb
"""
    if case.opcode == 3:
        assembly += f"mov di,{offset}\n"
        for marker in case.markers:
            assembly += f"mov cx,{size}\nmov al,{marker}\nrep stosb\n"
    assembly += f"""
    xor ax,ax
    mov es,ax
    and byte [es:43eh],7fh
    mov di,442h
    mov cx,7
    mov al,0a5h
    rep stosb
    mov al,6
    out 0ah,al
    xor al,al
    out 0ch,al
    mov al,57h
    out 05h,al
    mov al,13h
    out 05h,al
    in al,08h
    mov ax,6000h
    mov es,ax
    mov bx,{offset}
    mov ax,{(case.opcode << 8) | case.count}
    mov cx,{case.sector}
    xor dx,dx
    int 13h
    pushf
    pop dx
    and dl,1
    cmp dl,{int(bool(case.status))}
    jne done
    cmp ah,{case.status}
    jne done
    cmp al,{case.completed}
    jne done
    inc byte [stage]
    in al,08h
    and al,4
    cmp al,{4 if tc else 0}
    jne done
    xor al,al
    out 0ch,al
    in al,05h
    cmp al,{dma_count & 255}
    jne done
    in al,05h
    cmp al,{dma_count >> 8}
    jne done
    xor ax,ax
    mov es,ax
    test byte [es:43eh],80h
    jnz done
    cmp byte [es:443h],{st1 if hardware else 0xa5}
    jne done
    cmp byte [es:444h],{case.st2 if hardware else 0xa5}
    jne done
    inc byte [stage]
    mov ax,6000h
    mov es,ax
    mov di,{offset}
"""
    for marker in case.markers:
        assembly += f"mov cx,{size}\nmov al,{marker}\nrepe scasb\njne done\n"
    assembly += f"""
    mov cx,{2048 - len(case.markers) * size}
    mov al,0a5h
    repe scasb
    jne done
    inc byte [stage]
"""
    return assembly


# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def run(args, directory, image, machine, checks, writable=False):
    """Reopen an image for every sequence, optionally replaying its overlay."""
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              SETUP + checks + FINISH, machine=machine, read_only=not writable,
              extra_commands=("config -set cpu cpu_cycles=20000",))


def check_machine(args, directory, extension, machine):
    """Exercise error counts, Verify, ordinary writes and duplicate-ID selection."""
    pcjr = machine == "pcjr"
    fields = ((1, 0xfb, "", 0x11), (2, 0xfb, "", 0x22),
              (3, 0xfb, "data", 0x33), (4, 0xf8, "", 0x44))
    image = directory / f"bios-{machine}.{extension}"
    original = encode_tracks(extension, [make_track(fields, 2)])
    image.write_bytes(original)
    cases = (
        Case(2, count=2, completed=2, markers=(0x11, 0x22)),
        Case(4, count=2, completed=2, markers=()),
        Case(2, sector=2, count=2, status=0x10, completed=1,
             markers=(0x22, 0x33), st1=0x20, st2=0x20),
        Case(4, sector=2, count=2, status=0x10, completed=1,
             markers=(), st1=0x20, st2=0x20),
        (Case(2, sector=4, status=4, completed=0, markers=(0x44,), st2=0x40)
         if pcjr else
         Case(2, sector=4, status=4, completed=0, markers=(), st1=4, st2=0x40)),
        Case(2, sector=8, status=4, completed=0, markers=(), st1=4),
        Case(3, count=2, completed=2, markers=(0x66, 0x66)),
    )
    run(args, directory, image, machine,
        "".join(request(case, pcjr) for case in cases), True)
    saved = Path(str(image) + ".overlay").read_bytes()
    run(args, directory, image, machine,
        request(Case(2, count=2, completed=2, markers=(0x66, 0x66)), pcjr)
        + request(Case(4, count=2, completed=2, markers=()), pcjr))
    assert Path(str(image) + ".overlay").read_bytes() == saved
    assert image.read_bytes() == original
    duplicate = directory / f"duplicate-{machine}.{extension}"
    original = encode_tracks(extension, [make_track(
        ((1, 0xfb, "", 0x11), (1, 0xfb, "", 0x77)), 2)])
    duplicate.write_bytes(original)
    run(args, directory, duplicate, machine,
        request(Case(3, markers=(0x66,)), pcjr), True)
    run(args, directory, duplicate, machine,
        request(Case(2, markers=(0x66,)), pcjr)
        + request(Case(2, markers=(0x77,)), pcjr))
    assert duplicate.read_bytes() == original
    for size_code in (1, 3):
        image = directory / f"size-{machine}-{size_code}.{extension}"
        original = encode_tracks(extension, [make_track(((1, 0xfb, "", 0x11),), size_code)])
        image.write_bytes(original)
        run(args, directory, image, machine,
            request(Case(3, markers=(0x66,)), pcjr, size_code), True)
        run(args, directory, image, machine,
            request(Case(2, markers=(0x66,)), pcjr, size_code)
            + request(Case(4, markers=()), pcjr, size_code))
        assert image.read_bytes() == original
    image = directory / f"boundary-{machine}.{extension}"
    original = encode_tracks(extension, [make_track(((1, 0xfb, "", 0x11),), 2)])
    image.write_bytes(original)
    case = Case(2) if pcjr else Case(2, status=9, completed=0, markers=())
    run(args, directory, image, machine, request(case, pcjr, offset=0xff00))
    assert image.read_bytes() == original
    # BIOS FIFO transfers must not leak SPECIFY's mode into a later direct
    # DMA Read Track command (used by Crossfire's PCjr compatibility path).
    checks = SETUP + request(Case(2), pcjr)
    checks += "in al,21h\nand al,0bfh\nout 21h,al\n"
    checks += "push cs\npop es\n"
    checks += direct_transfer(0x42, 0x11, False)
    checks += "mov al,[expected_mask]\nout 21h,al\n"
    checks += FINISH.replace("checked:\n", "") + direct_routines(False)
    run_guest(args.dosbox.resolve(), args.nasm, directory, image, checks,
              machine=machine, extra_commands=("config -set cpu cpu_cycles=20000",))
    print(f"{extension} {machine}: FDC results, partial counts, CRC/deleted/missing IDs, "
          "IRQ/keyboard, DMA/FIFO, sizes, wrapping and duplicate persistence passed", flush=True)


def main():
    """Run both machine adapters across all four writable track formats."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for extension in ("mfm", "scp", "86f", "tc"):
            for machine in ("pcjr", "tandy"):
                check_machine(args, directory, extension, machine)


if __name__ == "__main__":
    main()
