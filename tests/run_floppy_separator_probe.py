#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Read an off-speed flux field with long transition gaps and a valid CRC."""

import argparse
from binascii import crc_hqx
from pathlib import Path
from struct import pack, pack_into
from tempfile import TemporaryDirectory

from run_floppy_read_deleted_probe import make_track
from run_floppy_transfer_edges_probe import DMA, RESET, ROUTINES
from run_hxc_mfm_probe import MFM
from run_transcopy_layout_probe import run_guest


def make_cells():
    """Keep an ordinary ID and replace part of its data with sparse transitions."""
    cells = make_track(((1, 0xfb, "", 0x77),), 2)
    sync = cells.find(b"\x44\x89" * 3, cells.find(b"\x44\x89" * 3) + 6)
    start = sync + 8
    # Thirty-two twelve-cell intervals exceed all legal FM/MFM spacings.
    cells[start + 128:start + 176] = b"\x00\x10\x01" * 16
    bits = "".join(f"{byte:08b}" for byte in cells[start:start + 1024])
    payload = bytes(int(bits[i + 1:i + 16:2], 2) for i in range(0, len(bits), 16))
    crc = crc_hqx(b"\xa1" * 3 + b"\xfb" + payload, 0xffff)
    previous = payload[-1] & 1
    encoded = bytearray()
    for byte in pack(">H", crc):
        encoded.extend(MFM[previous][byte])
        previous = byte & 1
    cells[start + 1024:start + 1028] = encoded
    return cells, payload


def capture(cells, ticks):
    """Encode the same raw cells at a selected clock, with measured index time."""
    bits = "".join(f"{byte:08b}" for byte in cells)
    edges = [i + 1 for i, bit in enumerate(bits) if bit == "1"]
    flux = b"".join(pack(">H", (edge - previous) * ticks)
                    for edge, previous in zip(edges, [0] + edges[:-1]))
    image = bytearray(0x2b0)
    image[:9] = b"SCP" + bytes((0x10, 0x31, 1, 0, 0, 1))
    pack_into("<I", image, 16, len(image))
    image.extend(pack("<3sBIII", b"TRK", 0, len(bits) * ticks, len(flux) // 2, 16))
    image.extend(flux)
    pack_into("<I", image, 12, sum(image[16:]) & 0xffffffff)
    return image


def check(args, directory, cells, payload, ticks):
    """Compare bytes, CRC status, IRQ6, DMA terminal count and buffer guards."""
    image = directory / f"separator-{ticks}.scp"
    original = capture(cells, ticks)
    image.write_bytes(original)
    checks = RESET + "call read_id\ncmp byte [ids+1],0\njne done\n"
    commands = ""
    for opcode in (0x46, 0x42, 0x56):
        label = f"{opcode}"
        checks += DMA.replace("mov cx,512", "mov cx,528") + f"""
    in al,08h
    mov si,command_{label}
    mov cx,9
    call send_command
    call wait_result
    cmp byte [ids],0
    jne done
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    in al,08h
    and al,4
    cmp al,{0 if opcode == 0x56 else 4}
    jne done
    mov si,buffer
    mov di,{'untouched' if opcode == 0x56 else 'flux_expected'}
    mov cx,512
    repe cmpsb
    jne done
    mov cx,16
guard_{label}:
    lodsb
    cmp al,0a5h
    jne done
    loop guard_{label}
"""
        commands += f"command_{label} db {opcode},0,0,0,1,2,1,54h,0ffh\n"
    expected = ",".join(str(byte) for byte in payload)
    checks += ("jmp checked\n" + commands + f"flux_expected db {expected}\n"
               "untouched times 512 db 0a5h\n" + ROUTINES + "checked:\n")
    run_guest(args.dosbox.resolve(), args.nasm, directory, image, checks,
              extra_commands=("config -set cpu cpu_cycles=20000",))
    assert image.read_bytes() == original
    assert not Path(str(image) + ".overlay").exists()
    print(f"{ticks * 25} ns: Read ID/Data/Track and Verify, CRC, DMA/IRQ6 and guards passed",
          flush=True)


def main():
    """Compare fast and slow captures against their uniform nominal-rate control."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    cells, payload = make_cells()
    with TemporaryDirectory() as temporary:
        for ticks in (80, 73, 87):
            check(args, Path(temporary), cells, payload, ticks)


if __name__ == "__main__":
    main()
