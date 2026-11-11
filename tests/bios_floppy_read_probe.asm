; SPDX-License-Identifier: GPL-2.0-or-later

bits 16
org 100h

    mov ax,350eh
    int 21h
    mov [old_irq6],bx
    mov [old_irq6+2],es
    mov dx,irq6
    mov ax,250eh
    int 21h
    in al,21h
    mov [saved_mask],al
    or al,40h
    out 21h,al
    cld

    mov bx,1100h
    mov byte [sector_count],1
    call check_read
    inc byte [stage]
    mov bx,0fb00h
    mov byte [sector_count],2
    call check_read
    inc byte [stage]
    mov bx,0fb01h
    call check_read
    mov byte [stage],0
    jmp report

check_read:
    mov ax,6010h
    mov es,ax
    mov di,bx
    mov cx,1024
    mov al,0a5h
    rep stosb
    in al,08h
    mov byte [interrupts],0
    mov ah,02h
    mov al,[sector_count]
    mov cx,0203h
    mov dx,0100h | UNIT
    push bx
    int 13h
    pop bx
    pushf
    pop si
%ifdef TRACK_IMAGE
    cmp byte [stage],3
    je .boundary_error
%endif
    jmp .success
%ifdef TRACK_IMAGE
.boundary_error:
    test si,1
    jz report
    cmp ax,0900h
    jne report
    cmp byte [interrupts],0
    jne report
    in al,08h
    test al,04h
    jnz report
    mov di,bx
    mov cx,1024
    mov al,0a5h
    repe scasb
    jne report
    jmp .check_mask
%endif
.success:
    test si,1
    jnz report
    cmp ah,0
    jne report
    cmp al,[sector_count]
    jne report
    in al,08h
%ifdef TRACK_IMAGE
    cmp byte [interrupts],1
    jne report
    test al,04h
    jz report
%else
    cmp byte [interrupts],0
    jne report
    test al,04h
    jnz report
%endif
    mov di,bx
    mov cx,512
    mov al,53h
    repe scasb
    jne report
    mov cx,512
    mov al,0a5h
    cmp byte [sector_count],1
    je .second_sector
    mov al,54h
.second_sector:
    repe scasb
    jne report
.check_mask:
    in al,21h
    test al,40h
    jz report
    ret

report:
    mov al,[saved_mask]
    out 21h,al
    push ds
    lds dx,[old_irq6]
    mov ax,250eh
    int 21h
    pop ds
    mov dx,filename
    xor cx,cx
    mov ah,3ch
    int 21h
    jc .exit
    mov bx,ax
    mov dx,stage
    mov cx,1
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
.exit:
    mov ax,4c00h
    int 21h

irq6:
    inc byte [cs:interrupts]
    jmp far [cs:old_irq6]

old_irq6 dd 0
saved_mask db 0
interrupts db 0
sector_count db 0
stage db 1
filename db 'BIOS.OUT',0
