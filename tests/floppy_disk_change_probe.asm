; SPDX-License-Identifier: GPL-2.0-or-later

bits 16
org 100h

%define DOR (0ch | UNIT | (10h << UNIT))

    mov al,[82h]
    mov [filename+6],al
    sub al,'0'
    cmp al,4
    ja fail
    xor ah,ah
    mov bx,ax
    shl bx,1
    jmp [phases+bx]

initial:
    call select_drive
    call drain_reset
    mov bx,0080h
    call check_change
    mov ax,1600h
    mov dl,80h
    int 13h
    jnc fail
    cmp ah,01h
    jne fail
    jmp pass

inserted:
    call select_drive
    call check_inserted
    mov al,31h
    call read_sector
    xor bx,bx
    call check_change
    jmp pass

swapped:
    call select_drive
    call check_inserted
    mov dx,3f2h
    xor al,al
    out dx,al
    mov al,DOR
    out dx,al
    call check_inserted
    call drain_reset
    xor bl,bl
    call seek
    call check_inserted
    mov bl,1
    call seek
    xor bx,bx
    call check_change
    mov al,52h
    call read_sector
    jmp pass

ejected:
    call select_drive
    mov bx,8080h
    call check_change
    mov bl,2
    call seek
    mov bx,8080h
    call check_change
    jmp pass

reinserted:
    call select_drive
    call check_inserted
    mov al,31h
    call read_sector
    xor bx,bx
    call check_change
    jmp pass

select_drive:
    mov dx,3f2h
    mov al,DOR
    out dx,al
    ret

check_inserted:
    mov bx,8006h
    call check_change
    ; AH=16h and reads of DIR leave the latch set.
    call check_change
    ; Select the other drive to check that the latch is per drive.
    mov dx,3f2h
    mov al,0ch | (UNIT ^ 1) | (10h << (UNIT ^ 1))
    out dx,al
    mov dx,3f7h
    in al,dx
    test al,80h
    jnz fail
    call select_drive
    ret

; BL is the BIOS status and BH is the expected DIR change bit.
check_change:
    mov ax,1600h
    mov dl,UNIT
    int 13h
    pushf
    pop si
    cmp ah,bl
    jne fail
    test bl,bl
    jz .clear
    test si,1
    jz fail
    jmp .port
.clear:
    test si,1
    jnz fail
.port:
    push es
    mov ax,40h
    mov es,ax
    cmp [es:41h],bl
    pop es
    jne fail
    mov dx,3f7h
    in al,dx
    and al,80h
    cmp al,bh
    jne fail
    ret

read_sector:
    push ax
    mov ax,6000h
    mov es,ax
    mov bx,1000h
    mov ax,0201h
    mov cx,1
    mov dx,UNIT
    int 13h
    jc fail
    pop ax
    cmp [es:1000h],al
    jne fail
    ret

seek:
    mov dx,3f5h
    mov al,0fh
    out dx,al
    mov al,UNIT
    out dx,al
    mov al,bl
    out dx,al
    ; Wait for the command phase using the guest timer as a deadline.
    xor ax,ax
    int 1ah
    mov si,dx
    mov dx,3f4h
.wait:
    in al,dx
    and al,0d0h
    cmp al,80h
    je .done
    xor ax,ax
    int 1ah
    sub dx,si
    cmp dx,18
    jae fail
    mov dx,3f4h
    jmp .wait
.done:
    call sense_interrupt
    cmp al,bl
    jne fail
    ret

drain_reset:
    mov cx,4
.next:
    call sense_interrupt
    loop .next
    ret

sense_interrupt:
    mov dx,3f5h
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    ret

fail:
    mov byte [result],1
pass:
    mov dx,filename
    xor cx,cx
    mov ah,3ch
    int 21h
    jc .exit
    mov bx,ax
    mov dx,result
    mov cx,1
    mov ah,40h
    int 21h
    mov ah,3eh
    int 21h
.exit:
    mov ax,4c00h
    int 21h

phases dw initial,inserted,swapped,ejected,reinserted
filename db 'CHANGE0.OUT',0
result db 0
