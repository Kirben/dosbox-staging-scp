; SPDX-License-Identifier: GPL-2.0-or-later
; Measure cylinder-51 Read ID intervals on both heads with the PIT.
bits 16
org 100h
    push cs
    pop ds
    push cs
    pop es
    mov ax,0201h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    mov byte [stage],2
    in al,21h
    mov [saved_mask],al
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
    mov byte [stage],3
    mov al,3
    call send
    mov al,0dfh
    call send
    mov al,2
    call send
    mov byte [stage],4
    mov al,0fh
    call send
    xor al,al
    call send
    mov al,51
    call send
    mov byte [stage],5
    mov ah,86h
    mov cx,9
    mov dx,27c0h
    int 15h
    mov al,8
    call send
    mov di,buffer
    call receive
    stosb
    call receive
    mov [buffer+1],al
    mov byte [stage],6
    cmp al,51
    jne done
    cli
    mov al,34h
    out 43h,al
    xor al,al
    out 40h,al
    out 40h,al
    sti
    mov di,records
    mov byte [head],0
head_loop:
    mov bp,24
next_id:
    call stamp
    mov al,4ah
    call send
    mov al,[head]
    shl al,2
    call send
    mov bx,7
result_loop:
    call receive
    stosb
    dec bx
    jnz result_loop
    call stamp
    dec bp
    jnz next_id
    inc byte [head]
    cmp byte [head],2
    jb head_loop
    mov dx,name
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc done
    mov bx,ax
    mov dx,records
    mov cx,912
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
    mov byte [stage],0
done:
    mov al,[saved_mask]
    out 21h,al
    mov dx,report
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc exit
    mov bx,ax
    mov dx,stage
    mov cx,1
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
    mov dx,setupname
    xor cx,cx
    mov ax,3c00h
    int 21h
    jc exit
    mov bx,ax
    mov dx,buffer
    mov cx,2
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
exit:
    mov ax,4c00h
    int 21h
send:
    push ax
    mov dx,3f4h
    mov cx,0ffffh
wait_send:
    in al,dx
    and al,0c0h
    cmp al,80h
    je can_send
    loop wait_send
    pop ax
    jmp done
can_send:
    pop ax
    inc dx
    out dx,al
    ret
receive:
    mov cx,0ffffh
    mov dx,3f4h
wait_result:
    in al,dx
    and al,0c0h
    cmp al,0c0h
    je ready
    loop wait_result
    jmp done
ready:
    inc dx
    in al,dx
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
head db 0
saved_mask db 0ffh
stage db 1
name db 'TIMES.BIN',0
report db 'RESULT.BIN',0
setupname db 'SETUP.BIN',0
records times 912 db 0
buffer times 512 db 0
