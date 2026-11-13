#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check programmed-rate field decoding, DMA bounds and rate-switch recovery."""

import argparse
from pathlib import Path
from struct import pack_into, unpack_from
from tempfile import TemporaryDirectory

from run_floppy_read_deleted_probe import make_track
from run_floppy_transfer_edges_probe import DMA, RESET, ROUTINES, one_sector
from run_hxc_mfm_probe import MFM
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


def command_checks(selector, opcode, label, rate=250):
    """Read one 250-kbps track at the selected separator clock."""
    correct = (500, 300, 250, 1000)[selector] == rate
    data_read = opcode in (0x46, 0x42)
    checks = DMA.replace("mov cx,512", "mov cx,528") + f"""
    in al,08h
    mov dx,3f7h
    mov al,{selector}
    out dx,al
    inc byte [stage]
"""
    if opcode == 0x4a:
        checks += "    call read_id\n"
    else:
        checks += f"""
    mov si,command_{label}
    mov cx,9
    call send_command
    call wait_result
"""
    checks += f"""
    cmp byte [ids],{0 if correct else 0x40}
    jne done
    cmp byte [ids+1],{0 if correct else 1}
    jne done
    cmp byte [ids+2],0
    jne done
    in al,08h
    and al,4
    cmp al,{4 if correct and data_read else 0}
    jne done
    mov si,buffer
    mov cx,512
check_data_{label}:
    lodsb
    cmp al,{0x77 if correct and data_read else 0xa5}
    jne done
    loop check_data_{label}
    mov cx,16
check_guard_{label}:
    lodsb
    cmp al,0a5h
    jne done
    loop check_guard_{label}
"""
    return checks


def check_image(dosbox, nasm, directory, extension):
    """Switch wrong/correct rates repeatedly without resetting the controller."""
    image = directory / f"rate.{extension}"
    original = encode_tracks(extension, [one_sector(2)])
    image.write_bytes(original)
    checks, commands = RESET, ""
    for index, selector in enumerate((2, 0, 2, 1, 2, 3, 2)):
        for opcode in (0x4a, 0x46, 0x42, 0x56):
            label = f"{index}_{opcode}"
            checks += command_checks(selector, opcode, label)
            commands += f"command_{label} db {opcode},0,0,0,1,2,1,54h,0ffh\n"
    checks += "    jmp checked\n" + commands + ROUTINES + "checked:\n"
    run_guest(dosbox, nasm, directory, image, checks,
              extra_commands=("config -set cpu cpu_cycles=20000",))
    assert image.read_bytes() == original
    assert not Path(str(image) + ".overlay").exists()
    print(f".{extension}: Read ID, Read Data, Read Track and Verify rates, "
          "IRQ6, DMA/guards and recovery passed", flush=True)



def mixed_capture(extension):
    """Encode a 250-kbps ID and a 500-kbps ID within one physical turn."""
    slow = make_track(((1, 0xfb, "", 0x11),), 2)[:1600]
    bits = "".join(f"{byte:08b}" for byte in slow)
    stretched = "".join("0" + bit for bit in bits)
    cells = bytearray(int(stretched[i:i + 8], 2) for i in range(0, len(stretched), 8))
    cells.extend(make_track(((2, 0xfb, "", 0x22),), 2)[:1600])
    cells.extend(MFM[0][0x4e] * ((25000 - len(cells)) // 2))
    image = bytearray(encode_tracks(extension, [cells]))
    if extension == "mfm":
        pack_into("<H", image, 12, 500)
    elif extension == "tc":
        image[0x100] = 2
    elif extension == "86f":
        record = unpack_from("<I", image, 8)[0]
        pack_into("<H", image, record, 8)
    else:
        record = unpack_from("<I", image, 16)[0]
        duration, count, offset = unpack_from("<III", image, record + 4)
        pack_into("<I", image, record + 4, duration // 2)
        for position in range(record + offset, record + offset + count * 2, 2):
            value = unpack_from(">H", image, position)[0]
            assert value % 2 == 0
            pack_into(">H", image, position, value // 2)
        pack_into("<I", image, 12, sum(image[16:]) & 0xffffffff)
    return image


def check_mixed(dosbox, nasm, directory, extension):
    """Recognize different IDs when selecting different rates on one track."""
    image = directory / f"mixed.{extension}"
    original = mixed_capture(extension)
    image.write_bytes(original)
    checks, commands = RESET, ""
    for index, (selector, sector, success) in enumerate(
            ((2, 1, True), (0, 1, False), (0, 2, True),
             (2, 2, False), (2, 1, True))):
        checks += DMA + f"""
    in al,08h
    mov dx,3f7h
    mov al,{selector}
    out dx,al
    inc byte [stage]
    call read_id
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+5],{1 if selector == 2 else 2}
    jne done
    mov si,mixed_{index}
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids],{0 if success else 0x40}
    jne done
    cmp byte [ids+1],{0 if success else 4}
    jne done
    cmp byte [ids+2],0
    jne done
    in al,08h
    and al,4
    cmp al,{4 if success else 0}
    jne done
    mov si,buffer
    mov cx,512
mixed_data_{index}:
    lodsb
    cmp al,{sector * 0x11 if success else 0xa5}
    jne done
    loop mixed_data_{index}
"""
        commands += f"mixed_{index} db 46h,0,0,0,{sector},2,{sector},54h,0ffh\n"
    checks += "    jmp checked\n" + commands + ROUTINES + "checked:\n"
    run_guest(dosbox, nasm, directory, image, checks,
              extra_commands=("config -set cpu cpu_cycles=20000",))
    assert image.read_bytes() == original
    print(f".{extension}: mixed-rate IDs and payloads passed", flush=True)

def main():
    """Exercise each captured-track backend using unchanged read-only images."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        for extension in ("scp", "mfm", "86f", "tc"):
            check_image(args.dosbox.resolve(), args.nasm, Path(temp), extension)
            check_mixed(args.dosbox.resolve(), args.nasm, Path(temp), extension)


if __name__ == "__main__":
    main()
