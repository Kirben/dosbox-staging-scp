; SPDX-License-Identifier: GPL-2.0-or-later
bits 16
org 100h

%ifndef UNIT
%define UNIT 0
%endif
%ifndef SECTORS
%define SECTORS 9
%endif
%define DRIVE_HEAD (4 + UNIT)
%define DOR (0x0c | UNIT | (0x10 << UNIT))

    mov ax,250eh
    mov dx,irq6
    int 21h
    in al,21h
    mov [saved_imr],al
    and al,0bfh
    out 21h,al

    mov byte [stage],1
    mov dx,3f2h
    xor al,al
    out dx,al
    mov al,DOR
    out dx,al
    call wait_irq
    xor bx,bx
.reset_sense:
    mov al,8
    call send
    call receive
    mov ah,bl
    or ah,0c0h
    cmp al,ah
    jne finish
    call receive
    test al,al
    jnz finish
    inc bl
    cmp bl,4
    jne .reset_sense

    mov byte [stage],2
    mov si,recalibrate
    mov cx,2
    call command_and_irq
    mov bl,0
    call sense_seek

    mov byte [stage],3
    mov si,seek
    mov cx,3
    call command_and_irq
    mov bl,2
    call sense_seek

    mov byte [stage],4
    call read_id
    mov bl,[result+5]
%ifdef REORDERED
    xor bh,bh
    mov bl,[next_id+bx-1]
%else
    cmp bl,SECTORS
    jb .advance
    xor bl,bl
.advance:
    inc bl
%endif
    call read_id
    cmp bl,[result+5]
    jne finish

    mov byte [stage],5
    ; Channel 2, device to memory, physical destination 61200h.
    mov ax,6000h
    mov es,ax
    mov di,1200h
    mov cx,513
    xor ax,ax
    cld
    rep stosw
    mov al,6
    out 0ah,al
    out 0ch,al
    mov al,46h
    out 0bh,al
    xor al,al
    out 04h,al
    mov al,12h
    out 04h,al
    mov al,0ffh
    out 05h,al
    mov al,3
    out 05h,al
    mov al,6
    out 81h,al
    in al,08h
    mov al,2
    out 0ah,al

    mov si,read_data
    mov cx,9
    call command_and_irq
    call read_result
    cmp word [result],DRIVE_HEAD
    jne finish
    cmp byte [result+2],0
    jne finish
    cmp word [result+3],0102h
    jne finish
    cmp word [result+5],0204h
    jne finish

    mov byte [stage],6
    in al,08h
    test al,4
    jz finish
    in al,08h
    test al,4
    jnz finish
    mov di,1200h
    mov cx,512
    mov al,53h
    repe scasb
    jne finish
    mov cx,512
    mov al,54h
    repe scasb
    jne finish
    cmp word [es:di],0
    jne finish

    mov byte [stage],7
    ; Terminal count masks the channel. A new read must report overrun.
    mov si,read_data
    mov cx,9
    call command_and_irq
    call read_result
    cmp word [result],1040h + DRIVE_HEAD
    jne finish
    in al,08h
    test al,4
    jnz finish

    mov byte [stage],8
    ; Memory-to-device mode must not write any guest memory.
    mov al,4ah
    out 0bh,al
    mov al,2
    out 0ah,al
    mov si,read_data
    mov cx,9
    call command_and_irq
    call read_result
    cmp word [result],1040h + DRIVE_HEAD
    jne finish
    in al,08h
    test al,4
    jnz finish
    cmp word [es:1600h],0
    jne finish

    mov byte [stage],0
finish:
    mov al,[saved_imr]
    out 21h,al
    mov dx,file
    mov ax,3c00h
    xor cx,cx
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
    mov al,[stage]
    mov ah,4ch
    int 21h

read_id:
    mov si,id
    mov cx,2
    call command_and_irq
    call read_result
    cmp word [result],DRIVE_HEAD
    jne finish
    cmp byte [result+2],0
    jne finish
    cmp word [result+3],0102h
    jne finish
    cmp byte [result+6],2
    jne finish
    cmp byte [result+5],1
    jb finish
    cmp byte [result+5],SECTORS
    ja finish
    ret

sense_seek:
    mov al,8
    call send
    call receive
    mov ah,20h + UNIT
    test bl,bl
    jz .status
    or ah,4
.status:
    cmp al,ah
    jne finish
    call receive
    cmp al,bl
    jne finish
    ret

command_and_irq:
    mov byte [interrupts],0
.next:
    lodsb
    call send
    loop .next
wait_irq:
    mov dx,0ffffh
.poll:
    cmp byte [interrupts],0
    jne .arrived
    mov cx,64
.delay:
    loop .delay
    dec dx
    jnz .poll
    jmp finish
.arrived:
    cmp byte [interrupts],1
    jne finish
    ret

send:
    push dx
    mov dx,3f4h
    push ax
    in al,dx
    and al,0c0h
    cmp al,80h
    jne finish
    pop ax
    inc dx
    out dx,al
    pop dx
    ret

receive:
    push dx
    mov dx,3f4h
    in al,dx
    and al,0c0h
    cmp al,0c0h
    jne finish
    inc dx
    in al,dx
    pop dx
    ret

read_result:
    push es
    push ds
    pop es
    mov di,result
    mov cx,7
    cld
.next:
    call receive
    stosb
    loop .next
    pop es
    ret

irq6:
    push ax
    inc byte [cs:interrupts]
    mov al,20h
    out 20h,al
    pop ax
    iret

recalibrate db 7,UNIT
seek db 0fh,DRIVE_HEAD,2
id db 4ah,DRIVE_HEAD
read_data db 46h,DRIVE_HEAD,2,1,3,2,4,2ah,0ffh
file db 'FDC.OUT',0
stage db 0
saved_imr db 0
interrupts db 0
result times 7 db 0
%ifdef REORDERED
next_id db 2,4,5,3,6,7,8,9,1
%endif
