#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check disk boot discards DOS RAM while preserving the BIOS and boot sector."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import time

from run_86f_attachment_probe import encode_86f
from run_hxc_mfm_probe import encode, launch
from run_scp_attachment_probe import encode_scp
from run_transcopy_attachment_probe import encode_transcopy


SEED = """
bits 16
org 100h
    int 12h
    mov cl,6
    shl ax,cl
    dec ax
    mov es,ax
    mov word [es:14],0a55ah
    xor ax,ax
    mov es,ax
    mov word [es:5f0h],53a7h
    mov ax,4c00h
    int 21h
"""

BOOT = """
bits 16
org 7c00h
    cli
    xor ax,ax
    mov ds,ax
    cmp word [5f0h],53a7h
    jne done
    mov bx,[413h]
    mov cl,6
    shl bx,cl
    mov dx,60h
paragraph:
    cmp dx,7c0h
    jne scan
    add dx,20h
scan:
    cmp dx,bx
    jae checked
    mov es,dx
    xor di,di
    mov cx,8
    xor ax,ax
    repe scasw
    jne done
    inc dx
    jmp paragraph
checked:
    ; The preserved BIOS services must still write a separate report disk.
    mov es,ax
    mov word [8000h],0a712h
    mov bx,8000h
    mov ax,0301h
    mov cx,1
    mov dx,80h
    sti
    int 13h
    jc done
    mov ax,0201h
    mov cx,2
    mov dx,80h
    int 13h
done:
    cli
    hlt
    jmp done
times 510-($-$$) db 0
dw 0aa55h
"""


def assemble(nasm, directory, name, source):
    """Assemble a small real-mode boot or DOS program."""
    path = directory / f"{name}.asm"
    path.write_text(source, encoding="ascii")
    output = directory / f"{name}.bin"
    subprocess.run([nasm, "-f", "bin", path, "-o", output], check=True)
    return output.read_bytes()


def check_boot(dosbox, directory, image, machine):
    """Seed free RAM, boot, and wait for the BIOS-written success marker."""
    marker = directory / "marker.img"
    marker.write_bytes(bytes(4 * 16 * 63 * 512))
    command, env = launch(dosbox, directory, "memory", [
        f'mount c "{directory}"', "c:", "seed.com",
        f'mount 2 "{marker}" -t hdd -fs none -chs 4,16,63',
        f'mount 0 "{image}" -t floppy -fs none -ro', "boot -l a",
    ])
    config = command[-1]
    config.write_text(f"[dosbox]\nmachine={machine}\n" + config.read_text(),
                      encoding="ascii")
    with subprocess.Popen(command, cwd=directory, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
        try:
            deadline = time.monotonic() + 15
            while marker.read_bytes()[:2] != b"\x12\xa7":
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(f"{image.suffix} {machine}: boot memory check failed")
                time.sleep(0.05)
        finally:
            if process.poll() is None:
                process.terminate()
            process.communicate(timeout=10)


def main():
    """Exercise all supported formats and standard-PC, PCjr and Tandy handoffs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        (directory / "SEED.COM").write_bytes(assemble(args.nasm, directory, "seed", SEED))
        disk = bytearray(360 * 1024)
        disk[:512] = assemble(args.nasm, directory, "boot", BOOT)
        for extension, encoder in (("img", bytes), ("ima", bytes), ("scp", encode_scp),
                                   ("mfm", encode), ("86f", encode_86f),
                                   ("tc", encode_transcopy)):
            image = directory / f"memory.{extension}"
            original = encoder(disk)
            image.write_bytes(original)
            for machine in ("cga", "pcjr", "tandy"):
                check_boot(args.dosbox.resolve(), directory, image, machine)
                assert image.read_bytes() == original
                print(f"{extension} {machine}: clean conventional RAM, BIOS workspace, "
                      "boot sector and BIOS I/O passed", flush=True)


if __name__ == "__main__":
    main()
