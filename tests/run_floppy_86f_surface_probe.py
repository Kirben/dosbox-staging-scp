#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Preserve 86F surface noise and weak-sector overlay writes."""
import argparse
from pathlib import Path
from struct import unpack_from
from tempfile import TemporaryDirectory

from run_floppy_read_deleted_probe import make_track
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest

SAVE = """
    mov dx,output_name
    xor cx,cx
    mov ah,3ch
    int 21h
    jc done
    mov bx,ax
    mov dx,buffer
    mov cx,2048
    mov ah,40h
    int 21h
    mov dx,statuses
    mov cx,16
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
    jmp captured
output_name db 'READS.BIN',0
statuses times 4 dw 0
flags times 4 dw 0
captured:
"""


def read_samples(dosbox, nasm, directory, image, *, action="read"):
    """Save four BIOS payload/status samples, optionally after an attempted write."""
    write = action != "read"
    rejected = action == "reject"
    checks = ""
    if write:
        checks += """
    mov di,buffer
    mov cx,512
    mov al,66h
    rep stosb
    mov ax,0301h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
"""
        checks += ("    jnc done\n    cmp ah,3\n    jne done\n" if rejected
                   else "    jc done\n    cmp ah,0\n    jne done\n")
    for index in range(4):
        checks += f"""
    mov ax,0201h
    mov bx,buffer+{index * 512}
    mov cx,1
    xor dx,dx
    int 13h
    pushf
    pop word [flags+{index * 2}]
    mov [statuses+{index * 2}],ax
"""
    run_guest(dosbox, nasm, directory, image, checks + SAVE, read_only=not write)
    result = (directory / "READS.BIN").read_bytes()
    assert len(result) == 2064
    samples = [result[index * 512:(index + 1) * 512] for index in range(4)]
    statuses = [value >> 8 for value in unpack_from("<4H", result, 2048)]
    carry = [value & 1 for value in unpack_from("<4H", result, 2056)]
    return samples, statuses, carry


def surface_image(kind):
    """Mark payload data cells explicitly, without relying on invalid MFM clocks."""
    marker = 0 if kind == "hole" else 0xff
    cells = make_track(((1, 0xfb, "", marker),), 2)
    image = bytearray(encode_tracks("86f", [cells]))
    record = unpack_from("<I", image, 8)[0]
    sync = b"\x44\x89" * 3
    payload = cells.find(sync, cells.find(sync) + 6) + 8
    if kind != "normal":
        surface = bytearray(len(cells))
        surface[payload:payload + 32] = b"\x55" * 32
        swapped = bytearray(len(surface))
        swapped[0::2], swapped[1::2] = surface[1::2], surface[0::2]
        image[record + 10 + len(cells):] = swapped
    if kind == "protected-weak":
        image[6] |= 0x10
    return image


def check_noise(samples, statuses, carry, marker):
    """Keep noise inside the annotated payload and report its sampled CRC."""
    assert len(set(samples)) > 1
    for sample, status, failed in zip(samples, statuses, carry):
        assert sample[16:] == bytes([marker]) * 496
        assert status in (0, 0x10) and failed == int(status != 0)


def main():
    """Exercise normal, weak, hole and write-protected surface captures."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for kind in ("normal", "weak", "hole", "protected-weak"):
            image = directory / f"{kind}.86f"
            original = surface_image(kind)
            image.write_bytes(original)
            samples, statuses, carry = read_samples(
                args.dosbox.resolve(), args.nasm, directory, image)
            if kind == "normal":
                assert samples == [b"\xff" * 512] * 4
                assert statuses == carry == [0] * 4
            else:
                check_noise(samples, statuses, carry, 0 if kind == "hole" else 0xff)
                rejected = kind != "weak"
                samples, statuses, carry = read_samples(
                    args.dosbox.resolve(), args.nasm, directory, image,
                    action="reject" if rejected else "write")
                overlay = Path(str(image) + ".overlay")
                if rejected:
                    assert not overlay.exists()
                    check_noise(samples, statuses, carry, 0 if kind == "hole" else 0xff)
                else:
                    assert samples == [b"\x66" * 512] * 4
                    assert statuses == carry == [0] * 4
                    saved = overlay.read_bytes()
                    samples, statuses, carry = read_samples(
                        args.dosbox.resolve(), args.nasm, directory, image)
                    assert samples == [b"\x66" * 512] * 4
                    assert statuses == carry == [0] * 4
                    assert overlay.read_bytes() == saved
            assert image.read_bytes() == original
            print(f"{kind}: surface sampling, write policy and source preservation passed",
                  flush=True)


if __name__ == "__main__":
    main()
