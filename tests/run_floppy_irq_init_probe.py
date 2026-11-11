#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check initial IRQ6 delivery without a guest-side unmask workaround."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_transcopy_attachment_probe import encode_transcopy
from run_transcopy_layout_probe import run_guest


CHECKS = """
    in al,21h
    test al,40h
    jnz done
    mov [mask],al
    call bios_read
    jc done
    in al,21h
    test al,40h
    jnz done
    inc byte [stage]
    mov dx,3f2h
    mov al,1ch
    out dx,al
    mov dx,3f5h
    mov cx,4
drain_reset:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop drain_reset
    mov ax,350eh
    int 21h
    mov [old_vector],bx
    mov [old_vector+2],es
    push cs
    pop es
    mov dx,irq_handler
    mov ax,250eh
    int 21h
    mov dx,3f5h
    mov al,0fh
    out dx,al
    xor al,al
    out dx,al
    out dx,al
    push ds
    xor ax,ax
    mov ds,ax
    mov bx,[046ch]
wait_irq:
    cmp byte [cs:irq_seen],0
    jne seek_done
    mov ax,[046ch]
    sub ax,bx
    cmp ax,4
    jb wait_irq
seek_done:
    pop ds
    push ds
    lds dx,[old_vector]
    mov ax,250eh
    int 21h
    pop ds
    cmp byte [irq_seen],0
    je done
    inc byte [stage]
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    cmp al,20h
    jne done
    in al,dx
    cmp al,0
    jne done
    inc byte [stage]
    ; BIOS calls must still preserve a mask explicitly set by the guest.
    in al,21h
    or al,40h
    out 21h,al
    call bios_read
    pushf
    in al,21h
    mov ah,al
    mov al,[mask]
    out 21h,al
    popf
    jc done
    test ah,40h
    jz done
    jmp checked
bios_read:
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    ret
irq_handler:
    push ax
    mov byte [cs:irq_seen],1
    mov al,20h
    out 20h,al
    pop ax
    iret
irq_seen db 0
mask db 0
old_vector dd 0
checked:
"""


def main():
    """Exercise raw and track images on standard-PC machine types."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    disk = bytes(360 * 1024)
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension, encoder in (("img", None), ("tc", encode_transcopy)):
            image = directory / f"disk.{extension}"
            image.write_bytes(encoder(disk) if encoder else disk)
            for machine in ("cga", "svga_s3", "tandy"):
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          CHECKS, machine=machine)
                print(f"{machine} .{extension}: initial IRQ6, Seek and mask preservation passed")


if __name__ == "__main__":
    main()
