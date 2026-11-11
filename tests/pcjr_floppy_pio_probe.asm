; SPDX-License-Identifier: GPL-2.0-or-later
bits 16
org 100h

%ifndef DATA_SIZE
%define DATA_SIZE 512
%endif
%define MSR 0f4h
%define DATA 0f5h

%ifdef WATCHDOG
    call check_watchdog
%endif

    mov byte [stage],1
    xor al,al
    out 0f2h,al
    mov al,81h
    out 0f2h,al
    mov si,specify
    mov cx,3
    call send
    ; Leave channel 2 masked and clear its previous terminal count.
    mov al,6
    out 0ah,al
    in al,08h

    mov byte [stage],2
    mov si,read_command
    mov cx,9
    call send
%ifdef REPLAY
    mov bl,0a7h
%else
    mov bl,51h
%endif
    call read_sector
%ifdef REPLAY
    mov bl,0b8h
%else
    mov bl,52h
%endif
    call read_sector
    mov bl,80h
    call check_result

%ifndef REPLAY
    mov byte [stage],3
    mov si,write_command
    mov cx,9
    call send
    mov bl,0a7h
    call write_sector
    mov bl,0b8h
    call write_sector
    mov bl,80h
    call check_result

    mov byte [stage],4
    ; Stop the motor after the first byte of a write and restart it.
    mov si,interrupted_write
    mov cx,9
    call send
    mov bl,0ffh
    call write_byte
    mov al,80h
    out 0f2h,al
    mov al,81h
    out 0f2h,al
    mov bl,04h
    call check_result

    mov byte [stage],5
    ; A read that misses its first byte deadline reports overrun.
    mov si,read_command
    mov cx,9
    call send
    mov bl,10h
    call check_result
%ifdef TRACK_IMAGE
    mov byte [stage],11
    mov si,skip_deleted
    mov cx,9
    call send
    mov bl,53h
    call read_sector
    mov bl,80h
    mov byte [expected_st2],40h
    call check_result
    mov byte [expected_st2],0

    mov byte [stage],6
    mov si,format_command
    mov cx,6
    call send
    mov si,format_ids
    mov cx,8
.next_id_byte:
    lodsb
    mov bl,al
    call write_byte
    loop .next_id_byte
    xor bl,bl
    call check_result
    mov si,read_track
    mov cx,9
    call send
    mov bl,0e5h
    call read_sector
    call read_sector
    mov bl,80h
    call check_result
%else
    mov byte [stage],12
    mov si,read_timed
    mov cx,9
    call send
    ; At 300 RPM, eight sectors span about 172 ms from first to last byte.
    ; A full slot plus the byte time for each sector takes over 280 ms.
    mov ax,40h
    mov es,ax
    call wait_for_read_byte
    mov bp,[es:6ch]
    mov cx,8 * DATA_SIZE
.next_timed_byte:
    call wait_for_read_byte
    in al,DATA
    loop .next_timed_byte
    mov ax,[es:6ch]
    sub ax,bp
    cmp ax,3
    jb report
    cmp ax,4
    ja report
    mov bl,80h
    call check_result
%endif
%endif

    in al,08h
    test al,4
    jnz report
    mov byte [stage],0
    jmp report

send:
    lodsb
    out DATA,al
    loop send
    ret

read_sector:
    mov cx,DATA_SIZE
.next:
    call wait_for_read_byte
    in al,DATA
    cmp al,bl
    jne report
    loop .next
    ret

wait_for_read_byte:
    in al,MSR
    and al,0f0h
    cmp al,0d0h
    je report
    cmp al,0f0h
    jne wait_for_read_byte
    ret

write_sector:
    mov cx,DATA_SIZE
.next:
    call write_byte
    loop .next
    ret

write_byte:
    in al,MSR
    and al,0f0h
    cmp al,0d0h
    je report
    cmp al,0b0h
    jne write_byte
    mov al,bl
    out DATA,al
    ret

check_result:
    in al,MSR
    cmp al,0d0h
    jne check_result
    in al,DATA
    mov ah,al
    and ah,0c0h
    jz .normal
    cmp ah,40h
    jne report
    test bl,bl
    jz report
    jmp .status
.normal:
    test bl,bl
    jnz report
.status:
    in al,DATA
    cmp al,bl
    jne report
    in al,DATA
    cmp al,[expected_st2]
    jne report
    mov cx,4
.rest:
    in al,DATA
    loop .rest
    in al,MSR
    cmp al,80h
    jne report
    ret

%ifdef WATCHDOG
check_watchdog:
    mov ax,350eh
    int 21h
    mov [old_irq6],bx
    mov [old_irq6+2],es
    mov dx,watchdog_irq
    mov ax,250eh
    int 21h
    in al,21h
    mov [old_mask],al
    and al,0bfh
    out 21h,al
    mov byte [hooked],1

    mov byte [stage],7
    mov al,81h
    out 0f2h,al
    ; An enable with the trigger held high must not start the timer.
    mov al,0e1h
    out 0f2h,al
    mov cx,60
    call wait_ticks
    cmp byte [watchdog_count],0
    jne report

    mov byte [stage],8
    mov al,0a1h
    out 0f2h,al
    ; Simulate a guest stuck partway through sending Specify.
    mov al,03h
    out DATA,al
    mov cx,18
    call wait_ticks
    mov al,0e1h
    out 0f2h,al
    mov al,0a1h
    out 0f2h,al
    mov cx,40
    call wait_ticks
    cmp byte [watchdog_count],0
    jne report
    mov cx,20
    call wait_ticks
    cmp byte [watchdog_count],1
    jne report
    in al,MSR
    cmp al,90h
    jne report

    mov byte [stage],9
    ; Disable and reset in the guest, then prove a canceled timer stays quiet.
    xor al,al
    out 0f2h,al
    mov al,0e1h
    out 0f2h,al
    mov al,0a1h
    out 0f2h,al
    mov al,81h
    out 0f2h,al
    mov cx,60
    call wait_ticks
    cmp byte [watchdog_count],1
    jne report

    mov byte [stage],10
    mov al,0e1h
    out 0f2h,al
    mov al,0a1h
    out 0f2h,al
    mov cx,60
    call wait_ticks
    cmp byte [watchdog_count],2
    jne report
    mov al,81h
    out 0f2h,al
    ret

wait_ticks:
    push es
    mov ax,40h
    mov es,ax
    mov bx,[es:6ch]
.wait:
    mov ax,[es:6ch]
    sub ax,bx
    cmp ax,cx
    jb .wait
    pop es
    ret

watchdog_irq:
    push ax
    inc byte [cs:watchdog_count]
    mov al,20h
    out 20h,al
    pop ax
    iret

old_irq6 dd 0
old_mask db 0
hooked db 0
watchdog_count db 0
%endif

report:
%ifdef WATCHDOG
    cmp byte [hooked],0
    je .write_report
    mov al,[old_mask]
    out 21h,al
    push ds
    lds dx,[old_irq6]
    mov ax,250eh
    int 21h
    pop ds
.write_report:
%endif
    mov ax,3c00h
    mov dx,filename
    xor cx,cx
    int 21h
    jc .exit
    mov bx,ax
    mov ah,40h
    mov dx,stage
    mov cx,1
    int 21h
    mov ah,3eh
    int 21h
.exit:
    mov ax,4c00h
    int 21h

read_timed db 46h,0,0,0,1,2,8,2ah,0ffh
skip_deleted db 66h,0,0,0,3,2,4,2ah,0ffh
expected_st2 db 0
specify db 03h,0dfh,1
read_command db 46h,0,0,0,1,2,2,2ah,0ffh
write_command db 45h,0,0,0,1,2,2,2ah,0ffh
interrupted_write db 45h,0,0,0,3,2,3,2ah,0ffh
format_command db 4dh,4,2,2,54h,0e5h
format_ids db 0,1,1,2, 0,1,2,2
read_track db 42h,4,0,1,1,2,2,2ah,0ffh
filename db 'PIO.OUT',0
stage db 0
