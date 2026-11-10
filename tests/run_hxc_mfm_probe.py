#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify HxC MFM through DOS MOUNT, BOOT and the floppy controller."""

import argparse
from binascii import crc_hqx
import os
from pathlib import Path
from struct import pack, pack_into
import subprocess
from tempfile import TemporaryDirectory
import time

from run_raw_floppy_read_probe import run_probe


def mfm_word(previous, value):
    word = 0
    for bit in range(7, -1, -1):
        data = (value >> bit) & 1
        word = (word << 2) | ((not (previous or data)) << 1) | data
        previous = data
    return pack(">H", word)


MFM = tuple(tuple(mfm_word(p, v) for v in range(256)) for p in (0, 1))


def encode_track(disk, track):
    cells = bytearray()
    previous = 0

    def append(data):
        nonlocal previous
        for byte in data:
            cells.extend(MFM[previous][byte])
            previous = byte & 1

    def field(data):
        nonlocal previous
        append(bytes(12))
        cells.extend(b"\x44\x89" * 3)
        previous = 1
        append(data + pack(">H", crc_hqx(b"\xa1" * 3 + data, 0xffff)))

    append(b"\x4e" * 80)
    order = (1, 2, 4, 3, 5, 6, 7, 8, 9) if track == 5 else range(1, 10)
    for sector in order:
        field(bytes((0xfe, track // 2, track % 2, sector, 2)))
        append(b"\x4e" * 22)
        offset = (track * 9 + sector - 1) * 512
        field(b"\xfb" + disk[offset:offset + 512])
        append(b"\x4e" * 40)
    append(b"\x4e" * ((12500 - len(cells)) // 2))
    return cells


def encode(disk):
    image = bytearray(19 + 80 * 11)
    pack_into("<7sHBHHBI", image, 0, b"HXCMFM", 40, 2, 300, 250, 7, 19)
    for track in range(80):
        cells = encode_track(disk, track)
        pack_into("<HBII", image, 19 + 11 * track, track // 2, track % 2,
                  len(cells), len(image))
        image.extend(cells)
    return image


def launch(dosbox, directory, name, commands):
    config = directory / (name + ".conf")
    config.write_text("[sdl]\noutput=texture\n[mixer]\nnosound=true\n"
                      "[autoexec]\n" + "\n".join(commands) + "\n",
                      encoding="ascii")
    args = [dosbox, "--noprimaryconf", "--nolocalconf", "-conf", config]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    return args, env


def check_fat(dosbox, directory):
    disk = bytearray(360 * 1024)
    disk[:11] = b"\xeb\x3c\x90MSDOS5.0"
    pack_into("<HBHBHHBHHH", disk, 11, 512, 2, 1, 2, 16, 720, 0xfd, 2, 9, 2)
    disk[510:512] = b"\x55\xaa"
    for start in (512, 1536):
        disk[start:start + 6] = b"\xfd\xff\xff\xff\x0f\x00"
    disk[2560:2572] = b"CHECK   DAT\x20"
    pack_into("<HI", disk, 2560 + 26, 2, 512)
    disk[3072:3584] = b"\x31" * 512

    image = directory / "fat.MFM"
    original = encode(disk)
    image.write_bytes(original)
    args, env = launch(dosbox, directory, "fat", [
        f'mount c "{directory}"', "c:", f'mount a "{image}"',
        "copy a:\\check.dat copied.dat", "copy copied.dat a:\\new.dat",
        "if exist a:\\new.dat echo FAILED > BAD.TXT", "exit",
    ])
    subprocess.run(args, cwd=directory, env=env, timeout=30, check=True,
                   capture_output=True)
    files = {path.name.lower(): path for path in directory.iterdir()}
    assert files["copied.dat"].read_bytes() == b"\x31" * 512
    assert "bad.txt" not in files or not files["bad.txt"].read_bytes()
    assert image.read_bytes() == original
    print(".mfm automatic FAT MOUNT, file read and write rejection passed")


def check_boot(dosbox, nasm, directory):
    source = directory / "boot.asm"
    source.write_text("""
bits 16
org 7c00h
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov bx,8000h
    mov ax,0201h
    mov cx,2
    xor dh,dh
    int 13h
    jc done
    cmp word [8000h],1234h
    jne done
    mov ax,0301h
    mov cx,2
    xor dh,dh
    int 13h
    jnc done
    ; Report success to a separate writable BIOS hard disk.
    mov word [8000h],0a712h
    mov ax,0301h
    mov cx,1
    mov dx,0080h
    int 13h
    jc done
    ; Seeking to the next sector flushes the buffered write.
    mov ax,0201h
    mov cx,2
    mov dx,0080h
    int 13h
done:
    hlt
    jmp done
times 510-($-$$) db 0
dw 0aa55h
""", encoding="ascii")
    output = directory / "boot.bin"
    subprocess.run([nasm, "-f", "bin", source, "-o", output], check=True)
    disk = bytearray(360 * 1024)
    disk[:512] = output.read_bytes()
    disk[512:514] = b"\x34\x12"
    image = directory / "boot.mfm"
    original = encode(disk)
    image.write_bytes(original)
    marker = directory / "marker.img"
    for name, commands in (
        ("direct", [f'boot "{image}"']),
        ("mounted", [f'mount 0 "{image}" -t floppy -fs none', "boot -l a"]),
    ):
        marker.write_bytes(bytes(4 * 16 * 63 * 512))
        mount_marker = f'mount 2 "{marker}" -t hdd -fs none -chs 4,16,63'
        args, env = launch(dosbox, directory, name, [mount_marker, *commands])
        log = directory / (name + ".log")
        with log.open("wb") as stream:
            process = subprocess.Popen(args, cwd=directory, env=env,
                                       stdout=stream, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 10
                while marker.read_bytes()[:2] != b"\x12\xa7":
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(log.read_text())
                    time.sleep(0.05)
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)
        assert image.read_bytes() == original
    print(".mfm direct and pre-mounted BOOT, INT 13h read and write rejection passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        run_probe(dosbox, args.nasm, directory, "mfm", 0, 360, 9, encode)
        check_fat(dosbox, directory)
        check_boot(dosbox, args.nasm, directory)


if __name__ == "__main__":
    main()
