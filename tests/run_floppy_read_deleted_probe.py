#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise Read Deleted Data marks, CRC, skip, DMA and native PCjr PIO."""

import argparse
from binascii import crc_hqx
from dataclasses import dataclass
from pathlib import Path
from struct import pack, unpack
from tempfile import TemporaryDirectory

from run_floppy_format_index_probe import CLOCK_SETUP, DECLARATIONS
from run_floppy_transfer_edges_probe import DMA
from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest


@dataclass(frozen=True)
# The record describes both the wire command and its expected transfer.
# pylint: disable-next=too-many-instance-attributes
class Case:
    """One physical mark sequence and the expected command result."""
    name: str
    opcode: int
    fields: tuple
    marker: int | None
    status_1: int = 0
    status_2: int = 0
    size_code: int = 2
    short_count: int = 0


CASES = (
    Case("deleted", 0x4c, ((1, 0xf8, "", 0x22),), 0x22),
    Case("normal", 0x4c, ((1, 0xfb, "", 0x11),), 0x11, status_2=0x40),
    Case("skip", 0x6c, ((1, 0xfb, "", 0x11), (2, 0xf8, "", 0x22)),
         0x22, status_2=0x40),
    Case("all-skipped-track", 0x6c, ((1, 0xfb, "", 0x11),), None, 0x80, 0x40),
    Case("data-crc", 0x4c, ((1, 0xf8, "data", 0x33),), 0x33, 0x20, 0x20),
    Case("normal-crc", 0x4c, ((1, 0xfb, "data", 0x44),), 0x44, 0x20, 0x60),
    Case("id-crc", 0x4c, ((1, 0xf8, "id", 0x55),), None, 0x20),
    Case("missing-dam", 0x6c, ((1, 0, "", 0),), None, 1, 1),
    Case("read-data-control", 0x46, ((1, 0xf8, "", 0x22),), 0x22, status_2=0x40),
    Case("skip-control", 0x66, ((1, 0xf8, "", 0x22), (2, 0xfb, "", 0x11)),
         0x11, status_2=0x40),
    Case("short-tc", 0x4c, ((1, 0xf8, "", 0x22),), 0x22, short_count=128),
    Case("size-256", 0x4c, ((1, 0xf8, "", 0x22),), 0x22, size_code=1),
    Case("size-1024", 0x4c, ((1, 0xf8, "", 0x22),), 0x22, size_code=3),
)
RAW_CASES = (
    CASES[1],
    Case("all-skipped", 0x6c, ((1, 0xfb, "", 0x11),), None, 0x80, 0x40),
    Case("read-data-control", 0x46, ((1, 0xfb, "", 0x11),), 0x11),
)


def make_track(fields, size_code, *, lead_gap=80):
    """Encode real FB/F8 fields with optional damaged ID/data CRCs."""
    cells = bytearray()
    previous = 0

    def append(data):
        nonlocal previous
        for byte in data:
            cells.extend(MFM[previous][byte])
            previous = byte & 1

    def field(data, bad):
        nonlocal previous
        append(bytes(12))
        cells.extend(b"\x44\x89" * 3)
        previous = 1
        crc = crc_hqx(b"\xa1" * 3 + data, 0xffff) ^ int(bad)
        append(data + pack(">H", crc))

    append(b"\x4e" * lead_gap)
    for sector, mark, error, marker in fields:
        field(bytes((0xfe, 0, 0, sector, size_code)), error == "id")
        append(b"\x4e" * 22)
        if mark:
            field(bytes([mark]) + bytes([marker]) * (128 << size_code), error == "data")
        append(b"\x4e" * 40)
    append(b"\x4e" * ((12500 - len(cells)) // 2))
    return cells


def routines(pio):
    """Poll execution bytes separately from the seven-byte result phase."""
    port = 0xf4 if pio else 0x3f4
    return f"""
wait_result:
    mov di,buffer
    mov ecx,2000000
poll:
    mov dx,{port}
    in al,dx
    and al,0f0h
    cmp al,0d0h
    je result_ready
    cmp al,0f0h
    jne next_poll
    cmp di,buffer+4096
    jae done
    inc dx
    in al,dx
    stosb
    cmp di,buffer+DATA_SIZE
    jne next_poll
    push di
    mov di,timestamps
    call stamp
    pop di
next_poll:
    dec ecx
    jnz poll
    jmp done
result_ready:
    mov ax,di
    sub ax,buffer
    mov [pio_count],ax
    mov di,timestamps+6
    call stamp
    mov dx,{port + 1}
    mov di,ids
    mov cx,7
result_byte:
    in al,dx
    stosb
    loop result_byte
    ret
send_command:
    mov dx,{port + 1}
    mov cx,9
send_byte:
    lodsb
    out dx,al
    loop send_byte
    ret
drain_reset:
    mov dx,{port + 1}
    mov cx,4
drain:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop drain
    ret
ids times 7 db 0
pio_count dw 0
"""


def guest_checks(case, pio):
    """Check data, untouched guards, status, completion ID and IRQ/DMA state."""
    size = 128 << case.size_code
    count = case.short_count or size
    transferred = 0 if case.marker is None else count
    normal = not case.status_1 and (case.status_2 == 0 or bool(case.opcode & 0x20))
    status_1 = 0x80 if pio and normal else case.status_1
    status_0 = 0x40 if status_1 or not normal else 0
    end_sector = len(case.fields)
    cylinder = 1 if normal and not pio else 0
    result_id = 1 if normal and not pio else end_sector
    reset = ("mov dx,0f2h\nmov al,80h\nout dx,al\nmov al,81h\nout dx,al\n"
             if pio else "mov dx,3f2h\nmov al,1ch\nout dx,al\n")
    setup = DMA.replace("mov cx,512", f"mov cx,{size + 16}").replace(
        "mov al,0ffh\n    out 05h,al", f"mov al,{(count - 1) & 255}\n    out 05h,al").replace(
        "mov al,1\n    out 05h,al", f"mov al,{(count - 1) >> 8}\n    out 05h,al")
    checks = reset + "call drain_reset\n" + CLOCK_SETUP + setup
    if pio:
        checks += ("mov al,6\nout 0ah,al\nmov dx,0f5h\nmov al,3\nout dx,al\n"
                   "mov al,0dfh\nout dx,al\nmov al,3\nout dx,al\n")
    checks += f"""
    in al,08h
    push es
    xor ax,ax
    mov es,ax
    and byte [es:43eh],7fh
    pop es
    push di
    mov di,timestamps
    call stamp
    pop di
    mov si,command
    call send_command
    call wait_result
    cmp byte [ids],{status_0}
    jne done
    cmp byte [ids+1],{status_1}
    jne done
    cmp byte [ids+2],{case.status_2}
    jne done
    cmp byte [ids+3],{cylinder}
    jne done
    cmp byte [ids+4],0
    jne done
    cmp byte [ids+5],{result_id}
    jne done
    cmp byte [ids+6],{case.size_code}
    jne done
    inc byte [stage]
    cmp word [pio_count],{transferred if pio else 0}
    jne done
    in al,08h
    and al,4
    cmp al,{4 if transferred and not pio else 0}
    jne done
    push es
    xor ax,ax
    mov es,ax
    test byte [es:43eh],80h
    pop es
    {'jnz' if pio else 'jz'} done
    mov di,buffer
    mov cx,{transferred}
    jcxz guard
    mov al,{case.marker or 0}
    repe scasb
    jne done
guard:
    mov cx,{size + 16 - transferred}
    mov al,0a5h
    repe scasb
    jne done
"""
    if (pio and transferred) or case.name == "all-skipped-track":
        checks += """
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
"""
    checks += (f"\njmp checked\ncommand db {case.opcode},0,0,0,1,"
               f"{case.size_code},{end_sector},54h,0ffh\n")
    return checks + routines(pio).replace("DATA_SIZE", str(size)) + DECLARATIONS + "checked:\n"


def check_duplicates(args, directory, extension, pio):
    """Successive reads of one CHRN must follow physical duplicate order."""
    fields = ((1, 0xfb, "", 0x11), (1, 0xf8, "", 0x77), (2, 0xf8, "", 0x22))
    image = directory / f"duplicate.{extension}"
    # Leave enough index gap to load the head before selecting the first copy.
    track = make_track(fields, 2, lead_gap=256 if pio else 80)
    original = encode_tracks(extension, [track])
    image.write_bytes(original)
    first = guest_checks(CASES[1], pio).split("jmp checked", 1)[0]
    second = guest_checks(Case("duplicate", 0x4c, ((1, 0xf8, "", 0x77),), 0x77), pio)
    second = second.split("jmp checked", 1)[0]
    second = second[second.index("    mov di,buffer"):]
    second = second.replace("jcxz guard", "jcxz second_guard").replace("guard:", "second_guard:")
    body = (first + second + "jmp checked\ncommand db 4ch,0,0,0,1,2,1,54h,0ffh\n"
            + routines(pio).replace("DATA_SIZE", "512") + DECLARATIONS + "checked:\n")
    run_guest(args.dosbox.resolve(), args.nasm, directory, image, body,
              machine="pcjr" if pio else "svga_s3",
              extra_commands=("config -set cpu cpu_cycles=20000",))
    assert image.read_bytes() == original
    print(f"{extension} rotational normal/deleted duplicate IDs {'PIO' if pio else 'DMA'} passed",
          flush=True)


def check_delay(directory, extension, case, _pio):
    """Check skipped-mark recognition and the final two physical CRC bytes."""
    t0, p0, t1, p1 = unpack("<IHIH", (directory / "CLOCK.BIN").read_bytes())
    delay = (65536 * (t1 - t0) + p0 - p1) * 1000 / 1193182
    bounds = (4.2, 4.8) if case.name == "all-skipped-track" else (0.045, 0.13)
    assert bounds[0] < delay < bounds[1], (extension, case.name, delay)


def main():
    """Run physical-mark cases and normal-only raw-image controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for extension in ("img", "ima", "scp", "mfm", "86f", "tc"):
            cases = RAW_CASES if extension in ("img", "ima") else CASES
            for case in cases:
                if args.smoke and case.name not in ("deleted", "normal", "skip"):
                    continue
                image = directory / f"{case.name}.{extension}"
                original = (b"\x11" * 512 + bytes(360 * 1024 - 512)
                            if extension in ("img", "ima") else
                            encode_tracks(extension, [make_track(case.fields, case.size_code)]))
                image.write_bytes(original)
                for pio in (False, True):
                    if pio and case.short_count:
                        continue
                    (directory / "CLOCK.BIN").unlink(missing_ok=True)
                    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                              guest_checks(case, pio), machine="pcjr" if pio else "svga_s3",
                              extra_commands=("config -set cpu cpu_cycles=20000",))
                    if (pio and case.marker is not None) or case.name == "all-skipped-track":
                        check_delay(directory, extension, case, pio)
                    assert image.read_bytes() == original
                    assert not Path(str(image) + ".overlay").exists()
                    print(f"{extension} {case.name} {'PIO' if pio else 'DMA'} passed", flush=True)
            if not args.smoke and extension not in ("img", "ima"):
                for pio in (False, True):
                    check_duplicates(args, directory, extension, pio)


if __name__ == "__main__":
    main()
