; SPDX-License-Identifier: GPL-2.0-or-later
bits 16
org 100h
    push cs
    pop ds
    push cs
    pop es
    in al,21h
    mov [saved_mask],al
%ifdef WRITE
    mov ax,6000h
    mov es,ax
    xor di,di
    mov cx,512
    mov al,66h
    cld
    rep stosb
    mov ax,0301h
    xor bx,bx
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    push cs
    pop es
%endif
    in al,21h
    or al,40h
    out 21h,al
    mov dx,3f2h
    xor al,al
    out dx,al
    mov al,1ch
    out dx,al
    mov bp,4
drain_reset:
    mov al,8
    call send
    call receive
    call receive
    dec bp
    jnz drain_reset
    mov al,3
    call send
    mov al,0dfh
    call send
    mov al,2
    call send
    mov dx,3f7h
    mov al,2
    out dx,al
    mov byte [stage],2
next_read:
    mov ax,6000h
    mov es,ax
    xor di,di
    mov cx,528
    mov al,0cch
    cld
    rep stosb
    mov al,6
    out 0ah,al
    xor al,al
    out 0ch,al
    out 04h,al
    out 04h,al
    mov al,6
    out 81h,al
    mov al,0ffh
    out 05h,al
    mov al,1
    out 05h,al
    mov al,46h
    out 0bh,al
    mov al,2
    out 0ah,al
    mov si,command
    mov bp,9
send_command:
    lodsb
    call send
    dec bp
    jnz send_command
    mov ah,86h
    mov cx,3
    mov dx,0d090h
    int 15h
    mov di,[next_status]
    mov bp,7
read_status:
    call receive
    mov [di],al
    inc di
    dec bp
    jnz read_status
    mov [next_status],di
    push cs
    pop es
    push ds
    mov ax,6000h
    mov ds,ax
    xor si,si
    mov di,[cs:next_record]
    mov cx,528
    rep movsb
    pop ds
    mov [next_record],di
    dec byte [turns]
    jnz next_read
    mov byte [stage],0
done:
    mov al,[saved_mask]
    out 21h,al
    mov dx,data_name
    mov si,records
    mov bp,32*528
    call save
    mov dx,status_name
    mov si,statuses
    mov bp,32*7
    call save
    mov dx,result_name
    mov si,stage
    mov bp,1
    call save
    mov ax,4c00h
    int 21h
save:
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc save_done
    mov bx,ax
    mov dx,si
    mov cx,bp
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
save_done:
    ret
send:
    push ax
    mov dx,3f4h
    mov cx,0ffffh
send_wait:
    in al,dx
    and al,0c0h
    cmp al,80h
    je send_ready
    loop send_wait
    pop ax
    jmp done
send_ready:
    pop ax
    inc dx
    out dx,al
    ret
receive:
    mov dx,3f4h
    mov cx,0ffffh
receive_wait:
    in al,dx
    and al,0c0h
    cmp al,0c0h
    je receive_ready
    loop receive_wait
    jmp done
receive_ready:
    inc dx
    in al,dx
    ret
command db READ_COMMAND,0,0,0,1,2,1,1ah,0ffh
saved_mask db 0ffh
stage db 1
turns db 32
next_status dw statuses
next_record dw records
data_name db 'DATA.BIN',0
status_name db 'STATUS.BIN',0
result_name db 'RESULT.BIN',0
statuses times 32*7 db 0
records times 32*528 db 0
