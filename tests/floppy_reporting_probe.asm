; SPDX-License-Identifier: GPL-2.0-or-later
; Capture a guest-visible snapshot before and after stepping each drive.
bits 16
org 100h

    push cs
    pop ds
    cld
    int 11h
    mov [equipment],ax
    mov al,10h
    call cmos_read
    mov [drive_types],al
    mov al,14h
    call cmos_read
    mov [cmos_equipment],al

    ; The IDE master shares 3F7h with the FDC. IDENTIFY PACKET DEVICE
    ; confirms that there is a responding ATAPI drive, not an empty bus.
    mov dx,3f6h
    mov al,2                       ; Poll the device with its IRQ disabled.
    out dx,al
    mov dx,1f6h
    mov al,0a0h
    out dx,al
    inc dx
    mov al,0a1h
    out dx,al
    mov cx,0ffffh
.wait_ide:
    in al,dx
    test al,80h
    jnz .retry
    test al,8
    jnz .read_ide
.retry:
    loop .wait_ide
    jmp .probe
.read_ide:
    mov dx,1f0h
    in ax,dx
    mov [identify],ax
    mov cx,255
.drain_ide:
    in ax,dx
    loop .drain_ide
.probe:
    mov di,drives
    call probe_drive
    inc byte [unit]
    call probe_drive

    mov dx,filename
    mov ah,3ch
    xor cx,cx
    int 21h
    jc .exit
    mov bx,ax
    mov ah,40h
    mov dx,equipment
    mov cx,report_end-equipment
    int 21h
    mov ah,3eh
    int 21h
.exit:
    mov ax,4c00h
    int 21h

cmos_read:
    out 70h,al
    in al,71h
    ret

probe_drive:
    mov ax,0800h
    xor bx,bx
    xor cx,cx
    mov dl,[unit]
    int 13h
    setc byte [di]
    mov [di+1],ah
    mov [di+2],bl
    mov [di+3],cx
    mov [di+5],dx
    call change_status
    mov [di+7],ax
    mov [di+9],bl
    mov [di+10],bh
    cmp byte [di],0
    jne .next

    ; Selecting the first drive through the DOR can leave reset status pending.
    xor ax,ax
    mov dl,[unit]
    int 13h
    jc .read_error

    ; A successful read on cylinder 1 forces a step even if the BIOS
    ; recalibrated the drive to cylinder zero on its first access.
    mov ax,0201h
    mov cx,0107h
    call read_sector
    jc .read_error
    mov ax,0201h
    mov cx,7
    call read_sector
    jc .read_error
    mov al,[es:bx]
    mov [di+11],al
    call change_status
    mov [di+12],ax
    mov [di+14],bl
    mov [di+15],bh
    jmp .next
.read_error:
    mov [di+16],ah
.next:
    add di,17
    ret

read_sector:
    push word 3000h
    pop es
    mov bx,200h
    xor dx,dx
    mov dl,[unit]
    int 13h
    ret

change_status:
    mov dx,3f2h
    mov al,3ch
    or al,[unit]
    out dx,al
    mov ax,1600h
    xor dx,dx
    mov dl,[unit]
    int 13h
    setc al
    push ds
    push word 40h
    pop ds
    mov bh,[41h]
    pop ds
    mov dx,3f7h
    push ax
    in al,dx
    mov bl,al
    pop ax
    ret

filename db 'REPORT.BIN',0
unit db 0
equipment dw 0
drive_types db 0
cmos_equipment db 0
identify dw 0
drives times 34 db 0
report_end:
