#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check physical TransCopy table slots through guest BIOS reads."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_hxc_mfm_probe import launch
from run_transcopy_attachment_probe import encode_transcopy


def run_guest(dosbox, nasm, directory, image, checks):
    """Run BIOS checks and report the first failing stage."""
    source = directory / "probe.asm"
    source.write_text("""
bits 16
org 100h
    push cs
    pop ds
    push cs
    pop es
""" + checks + """
    mov byte [stage],0
done:
    mov dx,report
    xor cx,cx
    mov ah,3ch
    int 21h
    jc exit
    mov bx,ax
    mov dx,stage
    mov cx,1
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
exit:
    mov ax,4c00h
    int 21h
stage db 1
report db 'RESULT.BIN',0
align 16
buffer times 8192 db 0
""", encoding="ascii")
    subprocess.run([nasm, "-f", "bin", source, "-o", directory / "PROBE.COM"],
                   check=True)
    report = directory / "RESULT.BIN"
    report.unlink(missing_ok=True)
    command, env = launch(dosbox, directory, "layout", [
        f'mount c "{directory}"', f'mount 0 "{image}" -t floppy -fs none -ro',
        "c:", "probe.com", "exit",
    ])
    run = subprocess.run(command, cwd=directory, env=env, timeout=30,
                         capture_output=True, text=True, check=True)
    result = report.read_bytes() if report.exists() else b""
    if result != b"\0":
        raise RuntimeError(f"{image.name}: stage {result.hex()}\n"
                           f"{run.stdout}{run.stderr}")


def check_mapping(dosbox, nasm, directory):
    """Read distant cylinders on native single- and double-sided tables."""
    disk = bytearray(360 * 1024)
    checks = ""
    for stage, cylinder in enumerate((2, 38), 1):
        offset = cylinder * 2 * 9 * 512
        disk[offset:offset + 512] = bytes([cylinder]) * 512
        checks += f"""
    mov byte [stage],{stage}
    mov ax,0201h
    mov bx,buffer
    mov cx,{cylinder * 256 + 1}
    xor dx,dx
    int 13h
    jc done
    cmp ah,0
    jne done
    mov si,buffer
    mov cx,512
check_{stage}:
    lodsb
    cmp al,{cylinder}
    jne done
    loop check_{stage}
"""
    for heads in (1, 2):
        for step in (1, 2):
            image = directory / f"mapping-{heads}-{step}.tc"
            original = encode_transcopy(disk, heads=heads, step=step)
            image.write_bytes(original)
            run_guest(dosbox, nasm, directory, image, checks)
            assert image.read_bytes() == original
            print(f"TC {heads} head(s), step {step}: cylinders 2 and 38 passed")


def main():
    """Run the native table regression."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        check_mapping(args.dosbox.resolve(), args.nasm, Path(temp))


if __name__ == "__main__":
    main()
