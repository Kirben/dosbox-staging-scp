#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check BIOS motor bookkeeping used by Tandy boot loaders."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_fat_floppy_cache_probe import fat12
from run_hxc_mfm_probe import encode
from run_scp_attachment_probe import encode_scp
from run_transcopy_attachment_probe import encode_transcopy
from run_transcopy_layout_probe import run_guest


CHECKS = """
    xor ax,ax
    mov es,ax
    mov byte [es:43fh],0
    mov byte [es:440h],0
    ; Use a guest parameter table so the timeout cannot be hard-coded.
    mov word [es:78h],parameters
    mov [es:7ah],cs
    push cs
    pop es
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    xor ax,ax
    mov es,ax
    cmp byte [es:43fh],1
    jne done
    cmp byte [es:440h],70h
    ja done
    cmp byte [es:440h],6ch
    jb done
    ; Leeper treats the motor mask as its one-based boot drive.
    mov dl,[es:43fh]
    dec dl
    push cs
    pop es
    mov byte [stage],2
    mov ax,0201h
    mov cx,2708h
    int 13h
    jc done
    xor ax,ax
    mov es,ax
    mov byte [es:43fh],0a1h
    mov byte [es:440h],1
    push cs
    pop es
    mov byte [stage],3
    mov ax,0201h
    mov cx,1
    mov dx,1
    int 13h
    jc done
    xor ax,ax
    mov es,ax
    cmp byte [es:43fh],0a3h
    jne done
    cmp byte [es:440h],70h
    ja done
    cmp byte [es:440h],6ch
    jb done
    ; Expiry clears motor bits while retaining the other BIOS flags.
    cli
    mov byte [es:440h],1
    int 8
    cli
    cmp byte [es:43fh],0a0h
    jne done
    cmp byte [es:440h],0
    jne done
    sti
    jmp checked
parameters db 0dfh,2,70h,2,9,2ah,0ffh,50h,0f6h,15,8
checked:
"""


def main():
    """Exercise Tandy, PCjr and standard-PC BIOS track reads on both drives."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        disk = fat12()
        for extension, encoder in (
            ("scp", encode_scp), ("mfm", encode),
            ("86f", encode_86f), ("tc", encode_transcopy),
        ):
            image = directory / f"motor.{extension}"
            original = encoder(disk)
            image.write_bytes(original)
            for machine in ("tandy", "pcjr", "svga_s3"):
                run_guest(args.dosbox.resolve(), args.nasm, directory, image, CHECKS,
                          machine=machine,
                          extra_commands=(f'mount 1 "{image}" -t floppy -fs none -ro',))
                assert image.read_bytes() == original
                print(f"{extension} {machine}: A/B motor mask, loader drive, "
                      "timeout reload and expiry passed", flush=True)


if __name__ == "__main__":
    main()
