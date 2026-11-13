#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check PC-compatible TC types, per-track clocks and mixed-type boot media."""

import argparse
from pathlib import Path
from struct import pack_into
from tempfile import TemporaryDirectory

from run_floppy_fm_probe import fm_track
from run_floppy_read_deleted_probe import make_track
from run_floppy_transfer_edges_probe import DMA, RESET, ROUTINES
from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import MFM, check_boot, check_fat
from run_transcopy_attachment_probe import encode_transcopy
from run_transcopy_layout_probe import run_guest


def capture(disk_type, track_type, fm, selector):
    """Store native FM, MFM, or oversampled 360-RPM MFM cells in a TC table."""
    cells = fm_track(2) if fm else make_track(((3, 0xfb, "", 0x33),), 2)
    if not fm and selector == 0:
        cells.extend(MFM[0][0x4e] * ((25000 - len(cells)) // 2))
    elif not fm and selector == 1:
        # Type 03h captures DD media in a 360-RPM drive. Sample its
        # 300-kbps MFM transitions at the container's 1-MHz cell clock.
        bits = "".join(f"{byte:08b}" for byte in cells)
        sampled = ["0"] * (round(len(bits) * 5 / 3 / 8) * 8)
        for position, bit in enumerate(bits):
            if bit == "1":
                sampled[min(round((position + 1) * 5 / 3) - 1, len(sampled) - 1)] = "1"
        cells = bytearray(int("".join(sampled[i:i + 8]), 2)
                          for i in range(0, len(sampled), 8))
    image = bytearray(encode_tracks("tc", [cells]))
    image[0x100] = disk_type
    pack_into("<H", image, 0x705, (track_type << 8) | 9)
    return image


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def check_capture(dosbox, nasm, directory, disk_type, track_type, fm, selector):
    """Check commands, DMA, IRQ6 and source integrity at each selected clock."""
    image = directory / f"type-{disk_type:02x}-{track_type:02x}.tc"
    original = capture(disk_type, track_type, fm, selector)
    image.write_bytes(original)
    checks, commands = RESET, ""
    sector = 1 if fm else 3
    for stage, command in enumerate((0x0a, 0x06, 0x02, 0x16), 1):
        opcode = command if fm else command | 0x40
        count = 2 if command == 0x0a else 9
        checks += DMA.replace("mov cx,512", "mov cx,528") + f"""
    mov byte [stage],{stage}
    in al,08h
    mov dx,3f7h
    mov al,{selector}
    out dx,al
    mov si,command_{stage}
    mov cx,{count}
    call send_command
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    cmp byte [ids+5],{1 if command == 0x06 else sector}
    jne done
    in al,08h
    and al,4
    cmp al,{4 if command in (0x06, 0x02) else 0}
    jne done
    mov di,buffer
    mov cx,512
    mov al,{0x11 if fm else 0x33}
"""
        if command in (0x06, 0x02) and fm:
            checks += f"""
    cmp byte [buffer],22h
    jne payload_{stage}
    mov al,22h
payload_{stage}:
"""
        elif command not in (0x06, 0x02):
            checks += "mov al,0a5h\n"
        checks += f"""
    repe scasb
    jne done
    mov cx,16
guard_{stage}:
    cmp byte [di],0a5h
    jne done
    inc di
    loop guard_{stage}
"""
        commands += (f"command_{stage} db {opcode},0\n" if count == 2 else
                     f"command_{stage} db {opcode},0,0,0,{sector},2,{sector},54h,0ffh\n")
    run_guest(dosbox, nasm, directory, image,
              checks + "jmp checked\n" + commands + ROUTINES + "checked:\n",
              extra_commands=("config -set cpu cpu_cycles=20000",))
    assert image.read_bytes() == original
    assert not Path(str(image) + ".overlay").exists()
    print(f"TC disk={disk_type:02x} track={track_type:02x} FM={fm} "
          f"selector={selector}: ID/data/track/verify, DMA, IRQ6 and guards passed", flush=True)


def mixed_disk(disk):
    """Give the disk an FM type while its ordinary tracks explicitly select MFM."""
    image = bytearray(encode_transcopy(disk))
    image[0x100] = 5
    for slot in range(80):
        pack_into("<H", image, 0x705 + slot * 2, 0x0709)
    return image


def main():
    """Exercise all defined PC-compatible types and per-track overrides."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for disk_type, track_type, fm, selector in (
                (2, 0, False, 0), (3, 0, False, 1), (5, 0, True, 2),
                (7, 0, False, 2), (0x0c, 0, True, 2), (0xff, 0, False, 2),
                (5, 7, False, 2), (0xff, 5, True, 2)):
            check_capture(dosbox, args.nasm, directory, disk_type, track_type, fm, selector)
        check_boot(dosbox, args.nasm, directory, mixed_disk, "tc")
        check_fat(dosbox, directory, mixed_disk, "tc")


if __name__ == "__main__":
    main()
