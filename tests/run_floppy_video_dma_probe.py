#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check floppy DMA to mapped video memory, including planar readback."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_transfer_edges_probe import RESET, one_sector, program
from run_floppy_zero_sector_probe import encode_tracks
from run_transcopy_layout_probe import run_guest


PAGING_CHECK = """
    mov byte [stage],5
    call check_paged_dma
"""

PAGING_ROUTINES = """
check_paged_dma:
    mov ax,8000h
    mov es,ax
    mov byte [es:0ff0h],55h
    mov ax,VIDEO_SEGMENT
    mov es,ax
    mov byte [es:0ff0h],33h
    push cs
    pop es
    xor eax,eax
    mov ax,cs
    mov [real_jump+3],ax
    shl eax,4
    mov ebp,eax
    mov [gdt_code+2],ax
    mov [gdt_stack+2],ax
    shr eax,16
    mov [gdt_code+4],al
    mov [gdt_stack+4],al
    mov [gdt_code+7],ah
    mov [gdt_stack+7],ah
    lea eax,[ebp+gdt]
    mov [gdt_pointer+2],eax
    lea ebx,[ebp+page_space+4095]
    and ebx,0fffff000h
    mov [page_directory],ebx
    mov eax,ebx
    shr eax,4
    mov es,ax
    xor di,di
    xor eax,eax
    mov cx,1024
    rep stosd
    lea eax,[ebx+4096+3]
    mov [es:0],eax
    mov ax,es
    add ax,100h
    mov es,ax
    xor di,di
    mov eax,3
    mov cx,1024
identity_page:
    stosd
    add eax,4096
    loop identity_page
    mov dword [es:VIDEO_PAGE*4],80003h
    mov dword [es:0d0h*4],VIDEO_BASE+3
    push cs
    pop es
    mov byte [dma_mode],46h
    call setup_dma
    mov si,read_command
    mov cx,9
    cli
    call send_command
    lgdt [gdt_pointer]
    mov eax,cr0
    or al,1
    mov cr0,eax
    jmp 8:protected_entry
protected_entry:
    mov ax,10h
    mov ds,ax
    mov es,ax
    mov ax,18h
    mov ss,ax
    mov eax,[cs:page_directory]
    mov cr3,eax
    mov eax,cr0
    or eax,80000000h
    mov cr0,eax
    mov ecx,1000000
    mov dx,3f4h
poll_paged:
    in al,dx
    and al,0d0h
    cmp al,0d0h
    je paged_result
    a32 loop poll_paged
    jmp leave_paging
paged_result:
    cmp byte [dword 0d0ff0h],77h
    jne leave_paging
    cmp byte [dword VIDEO_BASE+0ff0h],55h
    jne leave_paging
    mov byte [cs:paging_ok],1
leave_paging:
    mov eax,cr0
    and eax,7fffffffh
    mov cr0,eax
    and al,0feh
    mov cr0,eax
real_jump:
    jmp 0:real_entry
real_entry:
    mov ax,cs
    mov ds,ax
    mov es,ax
    mov ss,ax
    sti
    cmp byte [paging_ok],1
    jne failed
    call wait_result
    call check_result
    ret
align 8
gdt:
    dq 0
gdt_code:
    dw 0ffffh,0
    db 0,09ah,0,0
    dq 00cf92000000ffffh
gdt_stack:
    dw 0ffffh,0
    db 0,092h,0,0
gdt_pointer dw 31
    dd 0
page_directory dd 0
paging_ok db 0
page_space times 12288 db 0
"""


def guest_checks(planar, written=False, paging=False):
    """Cross a video-memory page with BIOS and direct DMA reads and writes."""
    segment = 0xa000 if planar else 0xb800
    mode = 0x0d if planar else 3
    expected = 0xa6 if written else 0x77
    setup = """
    mov dx,3c4h
    mov ax,0102h
    out dx,ax
    mov dx,3ceh
    mov ax,0004h
    out dx,ax
""" if planar else ""
    body = f"""
    mov ax,{mode}
    int 10h
{setup}
    mov byte [stage],2
    mov ax,{segment}
    mov es,ax
    mov di,0fefh
    mov cx,514
    mov al,0a5h
    rep stosb
    mov ax,0201h
    mov bx,0ff0h
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    cmp ax,1
    jne done
    call check_video
"""
    if paging:
        body += PAGING_CHECK
    if not written:
        body += RESET + """
    mov byte [stage],3
    mov byte [dma_mode],46h
    call setup_dma
    mov si,read_command
    mov cx,9
    call send_command
    call wait_result
    call check_result
    call check_video
    mov byte [stage],4
    mov ax,VIDEO_SEGMENT
    mov es,ax
    mov di,0ff0h
    mov cx,512
    mov al,0a6h
    rep stosb
    push cs
    pop es
    mov byte [dma_mode],4ah
    call setup_dma
    mov si,write_command
    mov cx,9
    call send_command
    call wait_result
    call check_result
""".replace('VIDEO_SEGMENT', str(segment))
    body += """
    push cs
    pop es
    jmp verified
setup_dma:
    mov al,6
    out 0ah,al
    xor al,al
    out 0ch,al
    mov ax,DMA_OFFSET
    out 04h,al
    mov al,ah
    out 04h,al
    mov al,DMA_PAGE
    out 81h,al
    mov al,0ffh
    out 05h,al
    mov al,1
    out 05h,al
    mov al,[dma_mode]
    out 0bh,al
    mov al,2
    out 0ah,al
    ret
check_result:
    cmp byte [ids],0
    jne failed
    cmp word [ids+1],0
    jne failed
    in al,08h
    test al,4
    jz failed
    xor al,al
    out 0ch,al
    in al,05h
    cmp al,0ffh
    jne failed
    in al,05h
    cmp al,0ffh
    jne failed
    ret
check_video:
    push es
    mov ax,VIDEO_SEGMENT
    mov es,ax
    cmp byte [es:0fefh],0a5h
    jne failed
    cmp byte [es:11f0h],0a5h
    jne failed
    mov di,0ff0h
    mov cx,512
check_byte:
    mov al,[es:di]
    cmp al,EXPECTED
    jne failed
    inc di
    loop check_byte
    pop es
    ret
failed:
    jmp done
verified:
""".replace('DMA_OFFSET', str((segment * 16 + 0xff0) & 0xffff)).replace(
        'DMA_PAGE', str(segment >> 12)).replace('VIDEO_SEGMENT', str(segment)).replace(
        'EXPECTED', str(expected))
    commands = """
dma_mode db 46h
read_command db 46h,0,0,0,1,2,1,54h,0ffh
write_command db 45h,0,0,0,1,2,1,54h,0ffh
"""
    if paging:
        commands += PAGING_ROUTINES.replace('VIDEO_SEGMENT', str(segment)).replace(
            'VIDEO_PAGE', str(segment >> 8)).replace('VIDEO_BASE', str(segment * 16))
    return program(body, commands)


def main():
    """Verify both DMA directions and persistent disk readback in a fresh guest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dosbox', required=True, type=Path)
    parser.add_argument('--nasm', default='nasm')
    args = parser.parse_args()
    for machine, planar in (('cga', False), ('tandy', False), ('svga_s3', True)):
        for extension in ('img', 'ima', 'tc', 'scp', 'mfm', '86f'):
            with TemporaryDirectory() as temp:
                directory = Path(temp)
                image = directory / f'disk.{extension}'
                original = (b'\x77' * (180 * 1024) if extension in ('img', 'ima')
                            else encode_tracks(extension, [one_sector(2)]))
                image.write_bytes(original)
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          guest_checks(planar, paging=extension == 'tc'),
                          machine=machine, read_only=False)
                run_guest(args.dosbox.resolve(), args.nasm, directory, image,
                          guest_checks(planar, written=True), machine=machine)
                if extension not in ('img', 'ima'):
                    assert image.read_bytes() == original
                    assert image.with_suffix(f'.{extension}.overlay').is_file()
                print(f'{machine} .{extension}: BIOS/direct video reads, DMA write, '
                      'page crossing, guards, IRQ6, terminal count and restart passed',
                      flush=True)


if __name__ == '__main__':
    main()
