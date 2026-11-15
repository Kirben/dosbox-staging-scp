#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Preserve interrupted writes and completed format fields on eject or replacement."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_fm_probe import fm_byte, fm_image, fm_track
from run_floppy_read_track_size_probe import ROUTINES
from run_floppy_transfer_edges_probe import DMA, RESET
from run_transcopy_layout_probe import run_guest


EXECUTE = """
    mov ax,ds
    mov [exec_block+4],ax
    mov bx,exec_block
    mov dx,mount_program
    mov ax,4b00h
    int 21h
    jc done
    push cs
    pop ds
    push cs
    pop es
"""

PREPARE = """
    mov sp,5000h
    mov bx,600h
    mov ah,4ah
    int 21h
    jc done
    in al,21h
    or al,40h
    out 21h,al
"""

WAIT_WRITE = """
    mov ebx,2000000
wait_write:
    mov dx,3f4h
    in al,dx
    and al,0f0h
    cmp al,0b0h
    je write_ready
    dec ebx
    jnz wait_write
    jmp done
write_ready:
"""


def program(body, data):
    """Keep commands and reusable polling routines outside the execution path."""
    return body + "\njmp checked\n" + data + ROUTINES + "checked:\n"


def interrupting_command(unit, replacement):
    """Construct the exact MOUNT tail executed while a media command is active."""
    tail = f' {unit} "{replacement}" -t floppy -fs none' if replacement else f' -u {unit}'
    return f"tail db {len(tail)},'{tail}',13\n"


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals
def interrupt(args, directory, image, control, unit, formatting, replace):
    """Remove the medium at a PIO boundary and prove the controller recovers."""
    running = 0x1c if unit == 0 else 0x2d
    body = PREPARE + RESET.replace("mov al,1ch", f"mov al,{running}")
    body += "mov si,specify\nmov cx,3\ncall send_command\n"
    if formatting:
        body += "mov si,configure\nmov cx,4\ncall send_command\n"
        body += "mov si,command\nmov cx,6\ncall send_command\n"
        body += "mov si,first_id\nmov cx,4\nnext_id_byte:\n" + WAIT_WRITE
        body += "inc dx\nlodsb\nout dx,al\nloop next_id_byte\n"
        # A second request means the first field has reached the disk.
        body += WAIT_WRITE.replace("wait_write", "wait_second").replace(
            "write_ready", "second_ready")
    else:
        body += "mov si,command\nmov cx,9\ncall send_command\n"
        body += "mov cx,64\nnext_data_byte:\n" + WAIT_WRITE
        body += "inc dx\nmov al,55h\nout dx,al\nloop next_data_byte\n"
    body += "mov byte [stage],2\n" + EXECUTE
    body += """
    call wait_result
    cmp byte [ids+1],4
    jne done
    mov byte [stage],3
    mov dx,3f4h
    in al,dx
    cmp al,80h
    jne done
"""
    if not replace:
        body += "mov word [exec_block+2],reinsert_tail\n" + EXECUTE
    body += RESET.replace("mov al,1ch", f"mov al,{running}")
    body += f"""
    mov dx,3f5h
    mov al,4ah
    out dx,al
    mov al,{unit}
    out dx,al
    call wait_result
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    inc byte [stage]
"""
    command = (f"command db 0dh,{unit},3,2,54h,0e5h\n" if formatting else
               f"command db 5,{unit},0,0,1,3,1,54h,0ffh\n")
    data = command + "specify db 3,0dfh,3\nconfigure db 13h,0,0,0\nfirst_id db 0,0,5,3\n"
    data += "exec_block dw 0,tail,0,0,0,0,0\nmount_program db 'Z:/MOUNT.COM',0\n"
    data += interrupting_command(unit, control if replace else None)
    data += interrupting_command(unit, control).replace("tail db", "reinsert_tail db")
    mounted = control if unit else image
    commands = ["config -set cpu cpu_cycles=20000"]
    if unit:
        commands.append(f'mount 1 "{image}" -t floppy -fs none')
    run_guest(args.dosbox.resolve(), args.nasm, directory, mounted,
              program(body, data), read_only=False, extra_commands=commands)


def readback(args, directory, image, formatting):
    """A fresh process must see the new prefix and the surviving second copy."""
    dma = DMA.replace("mov cx,512", "mov cx,1040").replace(
        "mov al,1\n    out 05h,al", "mov al,3\n    out 05h,al")
    body = RESET + dma + """
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    mov byte [stage],2
"""
    body += f"cmp byte [ids+1],{0 if formatting else 0x20}\njne done\n"
    body += f"cmp byte [ids+2],{0 if formatting else 0x20}\njne done\n"
    if formatting:
        body += "mov di,buffer\nmov cx,1024\nmov al,0e5h\nrepe scasb\njne done\n"
    else:
        body += "cmp byte [buffer],55h\njne done\ncmp byte [buffer+1023],11h\njne done\n"
    body += dma + """
    mov byte [command+4],1
    mov byte [command+6],1
    mov si,command
    mov cx,9
    call send_command
    call wait_result
    mov byte [stage],3
    cmp byte [ids+1],0
    jne done
    cmp byte [ids+2],0
    jne done
    mov di,buffer
    mov cx,1024
    mov al,22h
    repe scasb
    jne done
    mov al,0a5h
    mov cx,16
    repe scasb
    jne done
    inc byte [stage]
"""
    sector = 5 if formatting else 1
    run_guest(args.dosbox.resolve(), args.nasm, directory, image,
              program(body, f"command db 6,0,0,0,{sector},3,{sector},54h,0ffh\n"))


def main():
    """Test both drives, ejection and replacement for each track-image overlay."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        control = directory / "control.img"
        control.write_bytes(bytes(360 * 1024))
        for extension in ("scp", "mfm", "86f", "tc"):
            for unit in (0, 1):
                for formatting in (False, True):
                    for replace in (False, True):
                        image = directory / f"loss-{unit}-{formatting}-{replace}.{extension}"
                        original = fm_image(extension, (fm_byte(0xff) * 64 + fm_track(3))[:6250])
                        image.write_bytes(original)
                        interrupt(args, directory, image, control, unit, formatting, replace)
                        overlay = Path(str(image) + ".overlay")
                        saved = overlay.read_bytes()
                        readback(args, directory, image, formatting)
                        assert overlay.read_bytes() == saved
                        assert image.read_bytes() == original
                        assert control.read_bytes() == bytes(360 * 1024)
                        assert not Path(str(control) + ".overlay").exists()
                        print(f".{extension} drive {unit}, format={formatting}, replace={replace}: "
                              "progress, recovery, duplicate/suffix and restart passed", flush=True)


if __name__ == "__main__":
    main()
