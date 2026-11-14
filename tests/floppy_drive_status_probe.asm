; SPDX-License-Identifier: GPL-2.0-or-later

bits 16
org 100h

%define OTHER (UNIT ^ 1)
%define MOTOR_DOR (0ch | UNIT | (10h << UNIT))
%if PCJR
%define STATUS_PORT 0f4h
%define DATA_PORT 0f5h
%else
%define STATUS_PORT 3f4h
%define DATA_PORT 3f5h
%endif

%macro status 2-4 0,0ffh
    mov al,%1
    mov ah,%2
    mov bl,%3
    mov bh,%4
    call sense_drive
%endmacro

%macro seek 1
    mov dx,DATA_PORT
    mov al,0fh
    out dx,al
    mov al,UNIT
    out dx,al
    mov al,%1
    out dx,al
%endmacro

%macro recalibrate 0
    mov dx,DATA_PORT
    mov al,7
    out dx,al
    mov al,UNIT
    out dx,al
%endmacro

%macro sense_interrupt 2
    mov dx,DATA_PORT
    mov al,8
    out dx,al
    in al,dx
    cmp al,%1
    jne done
    in al,dx
    cmp al,%2
    jne done
    inc byte [stage]
%endmacro

%macro wait_us 1
    mov ah,86h
    mov cx,(%1 >> 16)
    mov dx,(%1 & 0ffffh)
    int 15h
%endmacro

    push cs
    pop ds
    mov al,[82h]
    mov [filename+6],al
    sub al,'0'
    mov [phase],al
    xor ax,ax
    mov es,ax
%if !PCJR
    cli
    mov ax,[es:38h]
    mov [old_irq],ax
    mov ax,[es:3ah]
    mov [old_irq+2],ax
    mov word [es:38h],irq_handler
    mov [es:3ah],cs
    in al,21h
    mov [old_mask],al
    and al,0bfh
    out 21h,al
    sti
%endif
    mov dx,3f2h
    mov al,MOTOR_DOR
    out dx,al
    cmp byte [phase],0
    je empty
    cmp byte [phase],1
    je writable
    cmp byte [phase],2
    je protected
    cmp byte [phase],3
    je ejected
    cmp byte [phase],4
    je reinserted
    jmp done

empty:
    call drain_reset
    status UNIT,38h | UNIT
    status UNIT | 4,3ch | UNIT
    ; BIOS slots 2/3 are not floppy drives, even with a read-only HDD mounted.
    status 2,2ah
    status 6,2eh
    status 3,2bh
    status 7,2fh
    jmp motor_off

writable:
    status UNIT,38h | UNIT
    status UNIT | 4,3ch | UNIT
    call other_drive
    ; A fast step rate makes the physical/PCN distinction inexpensive to test.
    mov dx,DATA_PORT
    mov al,3
    out dx,al
    mov al,0ffh
    out dx,al
    mov al,2
    out dx,al
    seek 80
    ; Do not prescribe TRACK0 while the head is moving.
    status UNIT,28h | UNIT,(1 << UNIT),0efh
    wait_us 200000
    status UNIT | 4,2ch | UNIT,(1 << UNIT)
    sense_interrupt (20h | UNIT),80
    recalibrate
    wait_us 200000
    ; Recalibrate clears PCN, but 79 pulses leave the head at cylinder 1.
    status UNIT,28h | UNIT,(1 << UNIT)
    sense_interrupt (70h | UNIT),0
    seek 0
    wait_us 1000
    status UNIT,28h | UNIT,(1 << UNIT)
    sense_interrupt (20h | UNIT),0
    recalibrate
    wait_us 20000
    status UNIT | 4,3ch | UNIT,(1 << UNIT)
    sense_interrupt (20h | UNIT),0
    status UNIT,38h | UNIT
    jmp motor_off

protected:
    status UNIT,78h | UNIT
    status UNIT | 4,7ch | UNIT
    seek 2
    wait_us 20000
    status UNIT,68h | UNIT,(1 << UNIT)
    sense_interrupt (20h | UNIT),2
    ; Controller reset preserves the physical head position.
    mov dx,3f2h
    xor al,al
    out dx,al
    mov al,MOTOR_DOR
    out dx,al
    call drain_reset
    status UNIT | 4,6ch | UNIT
    jmp motor_off

ejected:
    ; Ejecting media changes WP, not the physical head position.
    status UNIT,28h | UNIT
    status UNIT | 4,2ch | UNIT
    call other_drive
    jmp motor_off

reinserted:
    status UNIT,28h | UNIT
    status UNIT | 4,2ch | UNIT
    recalibrate
    wait_us 20000
    status UNIT,38h | UNIT,(1 << UNIT)
    sense_interrupt (20h | UNIT),0
    jmp motor_off

motor_off:
    mov dx,3f2h
    mov al,0ch | UNIT
    out dx,al
    ; 82077AA ST3 bits 5 and 3 remain one even with the motor stopped.
    mov ax,(38h | UNIT) << 8 | UNIT
    cmp byte [phase],2
    jne .not_protected
    mov ah,68h | UNIT
.not_protected:
    cmp byte [phase],3
    jne .not_ejected
    mov ah,28h | UNIT
.not_ejected:
    xor bl,bl
    mov bh,0ffh
    call sense_drive
    mov byte [stage],0
done:
%if !PCJR
    cli
    mov ax,[old_irq]
    mov [es:38h],ax
    mov ax,[old_irq+2]
    mov [es:3ah],ax
    mov al,[old_mask]
    out 21h,al
    sti
%endif
    push cs
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

other_drive:
    mov dx,3f2h
    mov al,0ch | OTHER
    out dx,al
    status OTHER,78h | OTHER
    status OTHER | 4,7ch | OTHER
    mov dx,3f2h
    mov al,MOTOR_DOR
    out dx,al
    ret

sense_drive:
    mov [request],al
    mov [expected],ah
    mov [busy],bl
    mov [status_mask],bh
    mov al,[irq_count]
    mov [saved_irq],al
%if !PCJR
    mov dx,3f7h
    in al,dx
    and al,80h
    mov [saved_change],al
%endif
    mov dx,STATUS_PORT
    in al,dx
    mov ah,[busy]
    or ah,80h
    cmp al,ah
    jne done
    mov dx,DATA_PORT
    mov al,4
    out dx,al
    mov dx,STATUS_PORT
    in al,dx
    mov ah,[busy]
    or ah,90h
    cmp al,ah
    jne done
    mov dx,DATA_PORT
    mov al,[request]
    out dx,al
    mov dx,STATUS_PORT
    in al,dx
    mov ah,[busy]
    or ah,0d0h
    cmp al,ah
    jne done
    mov dx,DATA_PORT
    in al,dx
    and al,[status_mask]
    cmp al,[expected]
    jne done
    mov dx,STATUS_PORT
    in al,dx
    mov ah,[busy]
    or ah,80h
    cmp al,ah
    jne done
    mov dx,DATA_PORT
    in al,dx
    cmp al,0ffh
    jne done
    wait_us 200
    mov al,[irq_count]
    cmp al,[saved_irq]
    jne done
%if !PCJR
    mov dx,3f7h
    in al,dx
    and al,80h
    cmp al,[saved_change]
    jne done
    in al,8
    test al,40h
    jnz done
%endif
    inc byte [stage]
    ret

drain_reset:
    mov dx,DATA_PORT
    mov cx,4
.next:
    mov al,8
    out dx,al
    in al,dx
    in al,dx
    loop .next
    ret

irq_handler:
    inc byte [cs:irq_count]
    jmp far [cs:old_irq]

phase db 0
stage db 1
request db 0
expected db 0
busy db 0
status_mask db 0
irq_count db 0
saved_irq db 0
saved_change db 0
old_mask db 0
old_irq dd 0
filename db 'STATUS0.OUT',0
