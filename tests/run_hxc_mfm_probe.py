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

from floppy_fixture_cache import cached_fixture

from run_raw_floppy_read_probe import run_probe


def mfm_word(previous, value):
    """Encode one byte and its clock bits as an MFM word."""
    word = 0
    for bit in range(7, -1, -1):
        data = (value >> bit) & 1
        word = (word << 2) | ((not (previous or data)) << 1) | data
        previous = data
    return pack(">H", word)


MFM = tuple(tuple(mfm_word(p, v) for v in range(256)) for p in (0, 1))


@cached_fixture
# pylint: disable-next=R0913,R0917,R0914
def encode_track(disk, track, duplicate_id=False, verify_errors=False,
                 malformed_id=False, deleted_sector=False):
    """Build a synthetic MFM track with optional protection cases."""
    cells = bytearray()
    previous = 0

    def append(data):
        nonlocal previous
        for byte in data:
            cells.extend(MFM[previous][byte])
            previous = byte & 1

    def field(data, bad_crc=False):
        nonlocal previous
        append(bytes(12))
        cells.extend(b"\x44\x89" * 3)
        previous = 1
        crc = crc_hqx(b"\xa1" * 3 + data, 0xffff) ^ int(bad_crc)
        append(data + pack(">H", crc))

    append(b"\x4e" * 80)
    order = (1, 2, 4, 3, 5, 6, 7, 8, 9) if track == 5 else range(1, 10)
    for sector in order:
        inject = verify_errors and track == 5
        if verify_errors and (track, sector) in ((5, 8), (6, 2)):
            continue
        sector_id = 6 if duplicate_id and track == 5 and sector == 5 else sector
        damaged = malformed_id and (track, sector) == (4, 1)
        size_code = 1 if damaged else 2
        field(bytes((0xfe, track // 2, track % 2, sector_id, size_code)),
              damaged or (inject and sector == 6))
        append(b"\x4e" * 22)
        offset = (track * 9 + sector - 1) * 512
        mark = b"\xf8" if deleted_sector and (track, sector) == (0, 3) else b"\xfb"
        field(mark + disk[offset:offset + 512], inject and sector == 4)
        append(b"\x4e" * 40)
    append(b"\x4e" * ((12500 - len(cells)) // 2))
    return cells


@cached_fixture
def encode(disk, verify_errors=False, malformed_id=False, deleted_sector=False):
    """Encode the complete disk as an HxC MFM image."""
    image = bytearray(19 + 80 * 11)
    pack_into("<7sHBHHBI", image, 0, b"HXCMFM", 40, 2, 300, 250, 7, 19)
    for track in range(80):
        cells = encode_track(disk, track, verify_errors=verify_errors,
                             malformed_id=malformed_id, deleted_sector=deleted_sector)
        pack_into("<HBII", image, 19 + 11 * track, track // 2, track % 2,
                  len(cells), len(image))
        image.extend(cells)
    return image


def launch(dosbox, directory, name, commands):
    """Write the guest configuration and return its command and environment."""
    config = directory / (name + ".conf")
    config.write_text("[sdl]\noutput=texture\n[mixer]\nnosound=true\n"
                      "[autoexec]\n" + "\n".join(commands) + "\n",
                      encoding="ascii")
    args = [dosbox, "--noprimaryconf", "--nolocalconf", "-conf", config]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    return args, env


def check_fat(dosbox, directory, encode_image=encode, extension="mfm", *, writable=True):
    """Read the fixture's marker file through DOS FAT."""
    disk = bytearray(360 * 1024)
    disk[:11] = b"\xeb\x3c\x90MSDOS5.0"
    pack_into("<HBHBHHBHHH", disk, 11, 512, 2, 1, 2, 16, 720, 0xfd, 2, 9, 2)
    disk[510:512] = b"\x55\xaa"
    for start in (512, 1536):
        disk[start:start + 6] = b"\xfd\xff\xff\xff\x0f\x00"
    disk[2560:2572] = b"CHECK   DAT\x20"
    pack_into("<HI", disk, 2560 + 26, 2, 512)
    disk[3072:3584] = b"\x31" * 512

    image = directory / f"fat.{extension.upper()}"
    original = encode_image(disk)
    image.write_bytes(original)
    args, env = launch(dosbox, directory, "fat", [
        f'mount c "{directory}"', "c:",
        f'mount a "{image}"' + (" -ro" if writable else ""),
        "copy a:\\check.dat copied.dat", "copy copied.dat a:\\new.dat",
        "if exist a:\\new.dat echo FAILED > BAD.TXT", "exit",
    ])
    subprocess.run(args, cwd=directory, env=env, timeout=30, check=True,
                   capture_output=True)
    files = {path.name.lower(): path for path in directory.iterdir()}
    assert files["copied.dat"].read_bytes() == b"\x31" * 512
    assert "bad.txt" not in files or not files["bad.txt"].read_bytes()
    assert image.read_bytes() == original
    print(f".{extension} automatic FAT MOUNT, file read and write rejection passed")
    if not writable:
        assert not image.with_suffix(f".{extension}.overlay").exists()
        return
    for name, commands in (
        ("fat-write", ["copy copied.dat a:\\new.dat"]),
        ("fat-readback", ["copy a:\\new.dat replay.dat"]),
    ):
        access = " -ro" if name == "fat-readback" else ""
        args, env = launch(dosbox, directory, name, [
            f'mount c "{directory}"', "c:", f'mount a "{image}"{access}',
            *commands, "exit",
        ])
        subprocess.run(args, cwd=directory, env=env, timeout=30, check=True,
                       capture_output=True)
    assert (directory / "REPLAY.DAT").read_bytes() == b"\x31" * 512
    assert image.read_bytes() == original
    print(f".{extension} DOS FAT file and allocation changes survived restart")


# pylint: disable-next=R0913,R0914
def check_boot(dosbox, nasm, directory, encode_image=encode, extension="mfm", *, writable=True):
    """Boot the fixture and check DMA and subsequent BIOS I/O."""
    track_image = extension not in ("img", "ima")
    source = directory / "boot.asm"
    source.write_text("""
bits 16
org 7c00h
    ; BOOT must have transferred the sector on DMA channel 2.
    in al,08h
    test al,4
    jz done
    out 0ch,al
    in al,04h
    cmp al,0
    jne done
    in al,04h
    cmp al,7eh
    jne done
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
    %ifdef TRACK_IMAGE
    mov ax,0401h
    int 13h
    jc done
    cmp word [8000h],1234h
    jne done
    mov ax,0401h
    mov cx,63
    int 13h
    jnc done
    cmp ax,0400h
    jne done
    cmp word [8000h],1234h
    jne done
    %endif
    mov word [8000h],5678h
    mov ax,0301h
    mov cx,2
    xor dh,dh
    int 13h
    %ifdef READ_ONLY
    jnc done
    cmp ax,0300h
    jne done
    %else
    jc done
    %endif
    ; Reading the written sector also flushes buffered raw-image writes.
    mov ax,0201h
    int 13h
    jc done
    %ifdef READ_ONLY
    cmp word [8000h],1234h
    %else
    cmp word [8000h],5678h
    %endif
    jne done
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
    options = ["-DTRACK_IMAGE"] if track_image else []
    if not writable:
        options.append("-DREAD_ONLY")
    subprocess.run([nasm, *options, "-f", "bin", source, "-o", output], check=True)
    disk = bytearray(360 * 1024)
    disk[:512] = output.read_bytes()
    disk[512:514] = b"\x34\x12"
    image = directory / f"boot.{extension}"
    original = encode_image(disk)
    image.write_bytes(original)
    marker = directory / "marker.img"
    boot_cases = [
        ("direct", [f'boot "{image}"']),
        ("mounted", [f'mount 0 "{image}" -t floppy -fs none', "boot -l a"]),
    ]
    overlay = image.with_suffix(f".{extension}.overlay")
    if track_image:
        boot_cases.append(("dos-path", [f'mount c "{directory}"', f"boot c:\\boot.{extension}"]))
    for name, commands in boot_cases:
        image.write_bytes(original)
        overlay.unlink(missing_ok=True)
        marker.write_bytes(bytes(4 * 16 * 63 * 512))
        mount_marker = f'mount 2 "{marker}" -t hdd -fs none -chs 4,16,63'
        args, env = launch(dosbox, directory, name, [mount_marker, *commands])
        log = directory / (name + ".log")
        with log.open("wb") as stream, subprocess.Popen(
            args, cwd=directory, env=env, stdout=stream, stderr=subprocess.STDOUT
        ) as process:
            try:
                deadline = time.monotonic() + 10
                while marker.read_bytes()[:2] != b"\x12\xa7":
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(log.read_text())
                    time.sleep(0.05)
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    raise
        if track_image:
            assert image.read_bytes() == original
            assert overlay.is_file() == writable
        else:
            expected = bytearray(original)
            expected[512:514] = b"\x78\x56"
            assert image.read_bytes() == expected
    print(f".{extension} direct and mounted BOOT completed DMA and BIOS I/O")


def check_overlay_format(dosbox, nasm, directory, extension, encoder):
    """Format a track, then read its new sector layout after a restart."""
    for mode in ("format-protected", "format", "format-readback"):
        run_probe(dosbox, nasm, directory, extension, 0, 360, 9, encoder,
                  write_mode=mode)


def main():
    """Run the HxC MFM attachment checks."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--boot-dma-raw", action="store_true")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        if args.boot_dma_raw:
            for extension in ("img", "ima"):
                check_boot(dosbox, args.nasm, directory, bytes, extension)
            return
        run_probe(dosbox, args.nasm, directory, "mfm", 0, 360, 9, encode)
        check_fat(dosbox, directory)
        check_boot(dosbox, args.nasm, directory)
        check_overlay_format(dosbox, args.nasm, directory, "mfm", encode)


if __name__ == "__main__":
    main()
