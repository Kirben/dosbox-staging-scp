; SPDX-License-Identifier: GPL-2.0-or-later
; Measure DOS, BIOS, and direct FDC reads with the same guest PIT clock.
bits 16
org 100h

start:
    push cs
    pop ds
    cld
    mov di,records
    ; Mode 2 is a down counter, so the latched count is a linear timestamp.
    cli
    mov al,34h
    out 43h,al
    xor al,al
    out 40h,al
    out 40h,al
    sti

    mov dx,file_name
    mov ax,3d00h
    int 21h
    jc fail
    mov bx,ax
    mov dx,file_buffer
    mov cx,4096
    mov ah,3fh
    call stamp
    int 21h
    call stamp
    jc fail
    cmp ax,4096
    jne fail
    cmp byte [file_buffer],31h
    jne fail
    cmp byte [file_buffer+4095],31h
    jne fail
    mov ah,3eh
    int 21h
    jc fail

    ; Align the physical DMA buffer and keep it inside a DMA page.
    mov ax,3000h
    mov es,ax
    mov bx,200h
    mov ax,0201h
    mov cx,7
    xor dx,dx
    int 13h
    jc fail

    call stop_motor
    mov ax,0201h
    mov cx,7
    call bios_read
    jc fail
    cmp ax,1
    jne fail
    cmp byte [es:bx],31h
    jne fail

    call stop_motor
    mov ax,0202h
    mov cx,9
    call bios_read
    jc fail
    cmp ax,2
    jne fail
    cmp byte [es:bx],69h
    jne fail
    cmp byte [es:bx+512],71h
    jne fail

    mov si,fdc_single
    mov bp,511
    call fdc_read
    cmp word [fdc_result],0
    jne fail
    cmp byte [fdc_result+2],0
    jne fail
    cmp byte [es:bx],31h
    jne fail

    mov si,fdc_cross
    mov bp,1023
    call fdc_read
    cmp word [fdc_result],4
    jne fail
    cmp byte [fdc_result+2],0
    jne fail
    cmp byte [es:bx],69h
    jne fail
    cmp byte [es:bx+512],71h
    jne fail

    mov dx,result_name
    jmp finish

bios_read:
    mov byte [es:bx],0cch
    xor dx,dx
    call stamp
    int 13h
    call stamp
    ret

stop_motor:
    mov dx,3f2h
    mov al,0ch
    out dx,al
    ret

; Reuse the BIOS-selected data rate and cylinder, then issue Read Data via ports.
; ES:BX is 3000h:0200h. BP is the DMA byte count minus one.
fdc_read:
    call stop_motor
    mov byte [es:bx],0cch
    mov byte [es:bx+512],0cch
    mov al,6
    out 0ah,al
    xor al,al
    out 0ch,al
    out 04h,al
    mov al,2
    out 04h,al
    mov ax,bp
    out 05h,al
    mov al,ah
    out 05h,al
    mov al,3
    out 81h,al
    mov al,46h
    out 0bh,al
    mov al,2
    out 0ah,al
    mov al,1ch
    out dx,al

    call stamp
    mov cx,9
.send:
    mov dx,3f4h
.wait_send:
    in al,dx
    and al,0c0h
    cmp al,80h
    jne .wait_send
    inc dx
    lodsb
    out dx,al
    loop .send

    mov si,fdc_result
    mov cx,7
.receive:
    mov dx,3f4h
.wait_receive:
    in al,dx
    and al,0c0h
    cmp al,0c0h
    jne .wait_receive
    inc dx
    in al,dx
    mov [si],al
    inc si
    loop .receive
    call stamp
    ret

stamp:
    pushf
    cli
    push eax
    push es
    push word 40h
    pop es
    mov eax,[es:6ch]
    mov [di],eax
    xor al,al
    out 43h,al
    in al,40h
    mov ah,al
    in al,40h
    xchg al,ah
    mov [di+4],ax
    add di,6
    pop es
    pop eax
    popf
    ret

fail:
    mov dx,fail_name
finish:
    cli
    mov al,36h
    out 43h,al
    xor al,al
    out 40h,al
    out 40h,al
    sti
    mov ah,3ch
    xor cx,cx
    int 21h
    jc .exit
    mov bx,ax
    mov ah,40h
    mov dx,records
    mov cx,di
    sub cx,dx
    int 21h
    mov ah,3eh
    int 21h
.exit:
    mov ax,4c00h
    int 21h

file_name db 'A:\CHECK.DAT',0
result_name db 'TIMES.BIN',0
fail_name db 'FAIL.BIN',0
fdc_single db 46h,0,0,0,7,2,7,1bh,0ffh
fdc_cross db 0c6h,0,0,0,9,2,9,1bh,0ffh
fdc_result times 7 db 0
records times 5*12 db 0
file_buffer times 4096 db 0
