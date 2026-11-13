#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise BIOS Format Track through PCjr FIFO and Tandy DMA."""

import argparse
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_bios_machine_probe import SETUP, FINISH, Case, request
from run_floppy_read_deleted_probe import make_track
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


@dataclass(frozen=True)
class Scenario:
    """One machine, drive and ID-table placement."""
    machine: str
    unit: int = 0
    size_code: int = 2
    offset: int = 0x2000
    status: int = 0
    read_only: bool = False
    invalid_size: bool = False


# pylint: disable-next=too-many-locals
def format_checks(case, order):
    """Assert the real format result, DMA consumption, IRQ6 and input guards."""
    pcjr = case.machine == "pcjr"
    count = len(order)
    transferred = case.status == 0
    tc = transferred and not pcjr
    code = SETUP + f"""
    mov byte [parameters+3],{case.size_code}
    mov byte [parameters+8],0e5h
    mov ax,6000h
    mov es,ax
    mov di,{(case.offset - 1) & 65535}
    mov cx,{count * 4 + 2}
    mov al,0a5h
    rep stosb
    mov di,{case.offset}
"""
    for sector in order:
        for value in (0, 0, sector, case.size_code + int(case.invalid_size)):
            code += f"mov al,{value}\nstosb\n"
    code += f"""
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
    mov bx,{case.offset}
    mov ax,{0x500 | count}
    mov cx,1
    mov dx,{case.unit}
    int 13h
    pushf
    pop dx
    and dl,1
    cmp dl,{int(bool(case.status))}
    jne done
    cmp ah,{case.status}
    jne done
    inc byte [stage]
    in al,08h
    and al,4
    cmp al,{4 if tc else 0}
    jne done
    xor al,al
    out 0ch,al
    in al,05h
    cmp al,{0xff if tc else 0x57}
    jne done
    in al,05h
    cmp al,{0xff if tc else 0x13}
    jne done
    xor ax,ax
    mov es,ax
    test byte [es:43eh],80h
    {'jz' if tc else 'jnz'} done
    inc byte [stage]
"""
    expected = (case.unit, 0, 0, 0, 0, order[-1], case.size_code)
    for index, value in enumerate(expected):
        code += f"cmp byte [es:{0x442 + index}],{value if transferred else 0xa5}\njne done\n"
    code += f"""
    mov ax,6000h
    mov es,ax
    cmp byte [es:{(case.offset - 1) & 65535}],0a5h
    jne done
    cmp byte [es:{(case.offset + count * 4) & 65535}],0a5h
    jne done
    mov di,{case.offset}
"""
    for sector in order:
        for value in (0, 0, sector, case.size_code + int(case.invalid_size)):
            code += f"mov al,{value}\nscasb\njne done\n"
    return code + "inc byte [stage]\n" + FINISH


def replay_checks(case, order):
    """Read the formatted sectors and check the physical cyclic ID order."""
    size = 128 << case.size_code
    code = SETUP + f"mov byte [parameters+3],{case.size_code}\n"
    for sector in order:
        code += f"""
    mov ax,6000h
    mov es,ax
    mov di,4000h
    mov cx,{size + 16}
    mov al,0a5h
    rep stosb
    mov bx,4000h
    mov ax,0201h
    mov cx,{sector}
    mov dx,{case.unit}
    int 13h
    jc done
    cmp ax,1
    jne done
    mov di,4000h
    mov cx,{size}
    mov al,0e5h
    repe scasb
    jne done
    mov cx,16
    mov al,0a5h
    repe scasb
    jne done
    inc byte [stage]
"""
    code += f"""
    ; Raw BIOS reads keep their sector path, so start the motor explicitly
    ; before testing the direct-controller ID sequence.
    mov dx,3f2h
    mov al,{0x0c | case.unit | (0x10 << case.unit)}
    out dx,al
    in al,21h
    and al,0bfh
    out 21h,al
    call read_id
    mov al,[result+5]
    mov si,order
    mov cx,{len(order)}
find_first:
    cmp al,[si]
    je found_first
    inc si
    loop find_first
    jmp done
found_first:
    mov bp,{len(order)}
next_id:
    cmp byte [result+1],0
    jne done
    cmp byte [result+2],0
    jne done
    cmp byte [result+3],0
    jne done
    cmp byte [result+4],0
    jne done
    cmp byte [result+6],{case.size_code}
    jne done
    mov al,[result+5]
    cmp al,[si]
    jne done
    inc si
    cmp si,order+{len(order)}
    jb id_advance
    mov si,order
id_advance:
    dec bp
    jz ids_done
    call read_id
    jmp next_id
ids_done:
    mov al,[expected_mask]
    out 21h,al
    inc byte [stage]
"""
    code += FINISH.replace("checked:\n", "")
    port = 0xf5 if case.machine == "pcjr" else 0x3f5
    return code + f"""
read_id:
    mov dx,{port}
    mov al,4ah
    out dx,al
    mov al,{case.unit}
    out dx,al
    dec dx
    mov ecx,2000000
poll:
    in al,dx
    and al,0d0h
    cmp al,0d0h
    je result_ready
    dec ecx
    jnz poll
    jmp done
result_ready:
    inc dx
    push cs
    pop es
    mov di,result
    mov cx,7
result_byte:
    in al,dx
    stosb
    loop result_byte
    ret
result times 7 db 0
order db {','.join(map(str, order))}
checked:
"""


# pylint: disable-next=too-many-locals
def check_image(args, directory, extension, case):
    """Use private mounts and reopen the image to prove persistence."""
    raw = extension in ("img", "ima")
    order = tuple(range(1, 10)) if raw else (3, 1, 2)
    label = f"{case.machine}-{case.unit}-{case.size_code}-{case.offset}-{case.status}"
    image = directory / f"format-{label}.{extension}"
    original = (b"\x5a" * (360 * 1024) if raw else
                encode_tracks(extension, [make_track(((1, 0xfb, "", 0x11),), 2)]))
    image.write_bytes(original)
    access = " -ro" if case.read_only else ""
    extra = ["config -set cpu cpu_cycles=20000"]
    if case.unit:
        extra.append(f'mount 1 "{image}" -t floppy -fs none{access}')
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              format_checks(case, order), machine=case.machine,
              read_only=case.read_only or bool(case.unit), extra_commands=tuple(extra))
    overlay = Path(str(image) + ".overlay")
    if case.status:
        assert image.read_bytes() == original
        assert not overlay.exists()
    else:
        saved = image.read_bytes() if raw else overlay.read_bytes()
        if raw:
            assert saved == b"\xe5" * (9 * 512) + original[9 * 512:]
        else:
            assert image.read_bytes() == original
        replay_extra = ["config -set cpu cpu_cycles=20000"]
        if case.unit:
            replay_extra.append(f'mount 1 "{image}" -t floppy -fs none -ro')
        run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                  replay_checks(case, order), machine=case.machine,
                  extra_commands=tuple(replay_extra))
        assert (image.read_bytes() if raw else overlay.read_bytes()) == saved
    print(f"{extension} {label}: Format Track status, FIFO/DMA, IRQ6, table and replay passed",
          flush=True)


def check_busy(args, directory, machine):
    """Reject a format during an incomplete command, then recover by reset."""
    image = directory / f"busy-{machine}.mfm"
    original = encode_tracks("mfm", [make_track(((1, 0xfb, "", 0x11),), 2)])
    image.write_bytes(original)
    order = (3, 1, 2)
    pending = "mov dx,3f5h\nmov al,0fh\nout dx,al\nxor al,al\nout dx,al\n"
    checks = SETUP + request(Case(2), machine == "pcjr")
    checks += pending + format_checks(Scenario(machine, status=0x80), order).replace(FINISH, "")
    checks += "xor ax,ax\nxor dx,dx\nint 13h\njc done\n"
    checks += format_checks(Scenario(machine), order)
    run_guest(args.dosbox.resolve(), args.nasm, directory, image, checks,
              machine=machine, read_only=False,
              extra_commands=("config -set cpu cpu_cycles=20000",))
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              replay_checks(Scenario(machine), order), machine=machine,
              extra_commands=("config -set cpu cpu_cycles=20000",))
    assert image.read_bytes() == original
    print(f"{machine}: busy format rejected, reset recovered, formatted data replayed", flush=True)


def main():
    """Run A/B mounts, sizes, table boundaries and rejection controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        if args.quick:
            check_image(args, directory, "mfm", Scenario("pcjr"))
            check_image(args, directory, "mfm", Scenario("tandy"))
            return
        for extension in ("mfm", "scp", "86f", "tc", "img", "ima"):
            raw = extension in ("img", "ima")
            for machine in ("pcjr", "tandy"):
                for unit in (0, 1):
                    check_image(args, directory, extension, Scenario(machine, unit))
                check_image(args, directory, extension,
                            Scenario(machine, status=3, read_only=True))
                check_image(args, directory, extension,
                            Scenario(machine, offset=0xfffe, status=0 if machine == "pcjr" else 9))
                if raw:
                    check_image(args, directory, extension,
                                Scenario(machine, status=4, invalid_size=True))
                else:
                    for size_code in (1, 3):
                        check_image(args, directory, extension,
                                    Scenario(machine, size_code=size_code))
            count = 9 if raw else 3
            check_image(args, directory, extension, Scenario("tandy", offset=65536 - count * 4))
        for machine in ("pcjr", "tandy"):
            check_busy(args, directory, machine)


if __name__ == "__main__":
    main()
