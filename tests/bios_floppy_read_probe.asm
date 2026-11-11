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
%ifdef VERIFY_TEST
    mov cx,0202h
    mov dx,0002h
    call check_verify
    inc byte [stage]
    mov cx,0203h
    mov dx,1001h
    call check_verify
    inc byte [stage]
    mov cx,0205h
    mov dx,1001h
    call check_verify
    inc byte [stage]
    mov cx,0207h
    mov dx,0401h
    call check_verify
    inc byte [stage]
    mov cx,0208h
    mov dx,0400h
    call check_verify
%else

    mov bx,1100h
    mov byte [sector_count],1
    call check_transfer
    inc byte [stage]
    mov bx,0fb00h
    mov byte [sector_count],2
    call check_transfer
    inc byte [stage]
%ifndef WRITE_READBACK
    mov bx,0fb01h
    call check_transfer
%endif
%ifdef WRITE_READBACK
    inc byte [stage]
    mov bx,1100h
    mov byte [sector_count],1
    call check_transfer
    inc byte [stage]
    call check_transfer
%endif
%endif
    mov byte [stage],0
    jmp report

%ifdef VERIFY_TEST
; CX contains CHS, DX the expected BIOS AX. Use a buffer crossing a DMA page.
check_verify:
    push dx
    mov ax,6010h
    mov es,ax
    mov bx,0fb01h
    mov di,bx
    push cx
    mov cx,1024
    mov al,0a5h
    rep stosb
    pop cx
    in al,08h
    mov byte [interrupts],0
    mov ax,0402h
    mov dx,0100h | UNIT
    int 13h
    pushf
    pop si
    pop dx
    cmp ax,dx
    jne report
    test dh,dh
    jz .success
    test si,1
    jz report
    jmp .check_result
.success:
    test si,1
    jnz report
.check_result:
    cmp byte [interrupts],1
    jne report
    in al,08h
    test al,04h
    jnz report
    mov di,0fb01h
    mov cx,1024
    mov al,0a5h
    repe scasb
    jne report
    in al,21h
    test al,40h
    jz report
    ret
%else
check_transfer:
    mov ax,6010h
    mov es,ax
    mov di,bx
    mov cx,1024
    mov al,0a5h
    rep stosb
%ifdef WRITE_TEST
    cmp byte [stage],3
    je .buffer_ready
    mov di,bx
    mov cx,512
    mov al,73h
    cmp byte [stage],1
    jne .fill_first
    mov al,75h
.fill_first:
    rep stosb
    cmp byte [sector_count],2
    jne .buffer_ready
    mov cx,512
    mov al,74h
    rep stosb
.buffer_ready:
%endif
    in al,08h
    mov byte [interrupts],0
    mov ah,02h
    mov al,[sector_count]
    mov cx,0203h
%ifdef WRITE_TEST
    mov ah,03h
    cmp byte [stage],1
    jne .issue
    mov cl,5
%elifdef WRITE_READBACK
    cmp byte [stage],4
    jb .issue
    mov cl,5
    je .issue
    mov ah,03h
%endif
.issue:
    mov dx,0100h | UNIT
    push bx
    int 13h
    pop bx
    pushf
    pop si
%ifdef WRITE_READBACK
    cmp byte [stage],5
    je .protected
%endif
%ifdef TRACK_IMAGE
%ifndef WRITE_READBACK
    cmp byte [stage],3
    je .boundary_error
%endif
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
%ifdef WRITE_READBACK
.protected:
    test si,1
    jz report
    cmp ax,0300h
    jne report
    cmp byte [interrupts],1
    jne report
    in al,08h
    test al,04h
    jnz report
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
%ifdef WRITE_TEST
    mov al,73h
    cmp byte [stage],1
    jne .check_first
    mov al,75h
%elifdef WRITE_READBACK
    mov al,73h
    cmp byte [stage],4
    jne .check_first
    mov al,75h
%endif
.check_first:
    repe scasb
    jne report
    mov cx,512
    mov al,0a5h
    cmp byte [sector_count],1
    je .second_sector
%ifdef WRITE_TEST
    mov al,74h
%elifdef WRITE_READBACK
    mov al,74h
%else
    mov al,54h
%endif
.second_sector:
    repe scasb
    jne report
.check_mask:
    in al,21h
    test al,40h
    jz report
    ret

%endif

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
