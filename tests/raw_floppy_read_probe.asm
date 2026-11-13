; SPDX-License-Identifier: GPL-2.0-or-later
bits 16
org 100h

%ifdef SCP_REVOLUTIONS
%define REVOLUTION_PROBE
%elifdef TC_WEAK_BITS
%define REVOLUTION_PROBE
%endif

%ifndef UNIT
%define UNIT 0
%endif
%ifndef SECTORS
%define SECTORS 9
%endif
%define DRIVE_HEAD (4 + UNIT)
%define DOR (0x0c | UNIT | (0x10 << UNIT))
%ifdef FORMAT_RESET
%define FORMAT_INTERRUPT FORMAT_RESET
%elifdef FORMAT_MOTOR
%define FORMAT_INTERRUPT FORMAT_MOTOR
%elifdef FORMAT_SWITCH
%define FORMAT_INTERRUPT 2
%endif

%ifdef BIOS_FORMAT
    mov ax,350eh
    int 21h
    mov [bios_irq6],bx
    mov [bios_irq6+2],es
%endif
    mov ax,250eh
    mov dx,irq6
    int 21h
    in al,21h
    mov [saved_imr],al
    and al,0bfh
    out 21h,al

%ifdef BIOS_FORMAT
%if UNIT = 0
    jmp bios_format_probe
%endif
%endif
    mov byte [stage],1
    call reset_and_seek
%ifdef TC_STEPPED
    ; Cylinder 1 is an empty physical slot in the step-2 capture.
    mov byte [stage],19
    mov si,seek_skipped
    mov cx,3
    call command_and_irq
    mov bl,1
    call sense_seek
    mov si,id
    mov cx,2
    call command_and_irq
    call read_result
    ; An empty stepped slot has no ID address mark.
    cmp word [result],0140h + DRIVE_HEAD
    jne finish
    mov si,seek
    mov cx,3
    call command_and_irq
    mov bl,2
    call sense_seek
%endif
%ifdef DUPLICATE_TEST
    jmp test_duplicate
%endif

    mov byte [stage],4
%ifdef FIRST_ID_AFTER_INDEX
    ; Restart the motor phase at index before asking for the first ID.
    mov dx,3f2h
    mov al,0ch | UNIT
    out dx,al
    mov al,DOR
    out dx,al
%endif
    call read_id
%ifdef FIRST_ID_AFTER_INDEX
    cmp byte [result+5],FIRST_ID_AFTER_INDEX
    jne finish
%endif
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
%ifdef SCP_FORMAT_READBACK
%ifdef FORMAT_PARTIAL
    mov bp,7
%else
    mov bp,SECTORS - 1
%endif
.check_formatted_ids:
    xor bh,bh
    mov bl,[result+5]
    mov bl,[next_id+bx-1]
    call read_id
    cmp bl,[result+5]
    jne finish
    dec bp
    jnz .check_formatted_ids
%endif

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
    ; Successful TC reports the next cylinder and sector after EOT.
    cmp word [result+3],0103h
    jne finish
    cmp word [result+5],0201h
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
%ifdef FORMAT_READBACK
    mov al,0e5h
%else
    mov al,53h
%endif
    repe scasb
    jne finish
    mov cx,512
%ifndef FORMAT_READBACK
    mov al,54h
%endif
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

%ifdef SCP_REVOLUTIONS
    mov byte [stage],9
    call read_turn_marker
    mov bl,[es:1200h]
    cmp bl,61h
    je .next_turn
    cmp bl,62h
    jne finish
.next_turn:
    call read_turn_marker
    mov al,[es:1200h]
    xor al,bl
    cmp al,3
    jne finish

    mov byte [stage],17
    call read_track_turn
    mov bl,[es:1200h + 5 * 512]
    cmp bl,61h
    je .next_track
    cmp bl,62h
    jne finish
.next_track:
    call read_track_turn
    mov al,[es:1200h + 5 * 512]
    xor al,bl
    cmp al,3
    jne finish
%endif

%ifdef TC_WEAK_BITS
    mov byte [stage],20
    mov bp,read_turn_marker
    mov bx,1200h
    call check_tc_weak
    inc byte [stage]
    mov bp,read_track_turn
    mov bx,1200h + 6 * 512
    call check_tc_weak
%endif

%ifdef WRITE_TEST
    ; Copy the existing DMA buffer to sectors 5 and 6.
    mov byte [stage],10
    mov al,6
    out 10,al
    mov al,0
    out 12,al
    mov al,74
    out 11,al
    mov al,0
    out 4,al
    mov al,18
    out 4,al
    mov al,255
    out 5,al
    mov al,3
    out 5,al
    mov al,2
    out 10,al
    mov si,write_data
    mov cx,9
    call command_and_irq
    call read_result
%ifdef WRITE_PROTECTED
    cmp word [result],0240h + DRIVE_HEAD
    jne finish
    in al,08h
    test al,4
    jnz finish
%else
    cmp word [result],DRIVE_HEAD
    jne finish
    cmp word [result+3],0103h
    jne finish
    cmp word [result+5],0201h
    jne finish
    in al,08h
    test al,4
    jz finish
%endif
%endif

%ifdef FORMAT_PARTIAL
%ifdef FORMAT_READBACK
    mov byte [stage],12
    ; Read the preserved sector write and its untouched neighbor.
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
    mov al,2
    out 0ah,al
    mov si,read_survivors
    mov cx,9
    call command_and_irq
    call read_result
    cmp word [result],DRIVE_HEAD
    jne finish
    cmp word [result+3],0103h
    jne finish
    cmp word [result+5],0201h
    jne finish
    mov di,1200h
    mov cx,512
    mov al,54h
    repe scasb
    jne finish
    mov cx,512
    xor al,al
    repe scasb
    jne finish
%endif
%endif

%ifdef FORMAT_TEST
%ifdef BIOS_FORMAT
bios_format_probe:
%endif
    mov byte [stage],11
%ifdef BIOS_FORMAT
    mov ax,351eh
    int 21h
    cmp byte [es:bx+8],0f6h
    jne finish
    mov byte [es:bx+8],0e5h
    mov byte [interrupts],0
    in al,08h
%ifndef FORMAT_PROTECTED
    ; A table at physical 6fff0h crosses the DMA page boundary.
    mov ax,6000h
    mov es,ax
    mov bx,0fff0h
    mov ax,0500h + SECTORS
    mov cx,0200h
    mov dx,0100h + UNIT
    int 13h
    jnc finish
    cmp ah,09h
    jne finish
    cmp byte [interrupts],0
    jne finish
%endif
    ; Alias 61000h with a wrapping segment offset on the same DMA page.
    mov ax,6000h
    mov es,ax
    mov di,1000h
    mov si,format_ids
    mov cx,SECTORS * 4
    cld
    rep movsb
    mov ax,5101h
    mov es,ax
    mov bx,0fff0h
    ; The BIOS must deliver IRQ6 even if the caller masked it.
    in al,21h
    or al,40h
    out 21h,al
    mov ax,0500h + SECTORS
    mov cx,0200h
    mov dx,0100h + UNIT
    int 13h
%ifdef FORMAT_PROTECTED
    jnc finish
    cmp ah,03h
%elifdef FORMAT_INVALID
    jnc finish
    cmp ah,04h
%else
    jc finish
    test ah,ah
%endif
    jne finish
    in al,21h
    test al,40h
    jz finish
%assign EXPECT_FORMAT_IRQ 1
%ifdef FORMAT_PROTECTED
%assign EXPECT_FORMAT_IRQ 0
%elifdef FORMAT_INVALID
%ifdef SCP_FORMAT
%assign EXPECT_FORMAT_IRQ 0
%endif
%endif
    cmp byte [interrupts],EXPECT_FORMAT_IRQ
    jne finish
    in al,08h
    test al,4
%if EXPECT_FORMAT_IRQ
    jz finish
    mov ax,0040h
    mov es,ax
    ; The BIOS consumes its completion flag; the IRQ counter above survives.
    test byte [es:003eh],80h
    jnz finish
    out 0ch,al
    in al,04h
    cmp al,SECTORS * 4
    jne finish
    in al,04h
    cmp al,10h
    jne finish
%else
    jnz finish
%endif
%else
    ; Channel 2 transfers four CHRN bytes per sector.
    mov di,1200h
    mov si,format_ids
    mov cx,SECTORS * 4
    rep movsb
    mov al,6
    out 0ah,al
    out 0ch,al
    mov al,4ah
    out 0bh,al
    xor al,al
    out 04h,al
    mov al,12h
    out 04h,al
%ifdef FORMAT_EARLY_TC
    ; Two complete IDs and one byte of the third before terminal count.
    mov al,8
%else
    mov al,SECTORS * 4 - 1
%endif
    out 05h,al
    xor al,al
    out 05h,al
    mov al,2
    out 0ah,al
    mov si,format_command
    mov cx,6
%ifdef FORMAT_INTERRUPT
%if FORMAT_INTERRUPT < 3
    call send_command
%if FORMAT_INTERRUPT = 2
    ; Wait for DMA to consume exactly two CHRN tuples.
    mov ebp,400000
.wait_for_two_ids:
    out 0ch,al
    in al,04h
    cmp al,8
    je .interrupt_now
    ja finish
    dec ebp
    jnz .wait_for_two_ids
    jmp finish
.interrupt_now:
%endif
    cmp byte [interrupts],0
    jne finish
    in al,08h
    test al,4
    jnz finish
    jmp format_interrupt_recovery
%endif
%endif
    call command_and_irq
    call read_result
%ifdef FORMAT_PROTECTED
    cmp word [result],0240h + DRIVE_HEAD
%elifdef FORMAT_INVALID
    cmp word [result],0440h + DRIVE_HEAD
%else
%ifdef FORMAT_PARTIAL
    cmp word [result],1040h + DRIVE_HEAD
%else
    cmp word [result],DRIVE_HEAD
%endif
    jne finish
    cmp word [result+3],0102h
    jne finish
%ifdef SCP_FORMAT
    cmp word [result+5],020bh
%else
    cmp word [result+5],0200h + SECTORS
%endif
%endif
    jne finish
    cmp byte [result+2],0
    jne finish
    in al,08h
    test al,4
%ifdef FORMAT_PROTECTED
    jnz finish
%else
    jz finish
%endif
%ifdef FORMAT_INTERRUPT
format_interrupt_recovery:
%ifdef FORMAT_RESET
    call reset_and_seek
%else
    mov dx,3f2h
%ifdef FORMAT_SWITCH
    mov al,DOR ^ 1
%else
    mov al,0ch | UNIT
%endif
    mov byte [interrupts],0
    out dx,al
%if FORMAT_INTERRUPT < 3
    ; The interruption completes the command before the next sector event.
    mov dx,3f4h
    in al,dx
    cmp al,0d0h
    jne finish
    call wait_irq
    call read_result
    cmp word [result],0440h + DRIVE_HEAD
    jne finish
    cmp byte [result+2],0
    jne finish
%if FORMAT_INTERRUPT = 2
    cmp word [result+3],0102h
    jne finish
    cmp word [result+5],020bh
    jne finish
%endif
%endif
    mov dx,3f2h
    mov al,DOR
    out dx,al
%endif
    mov byte [stage],13
    call read_id
%endif
%endif
%endif
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

%ifdef REVOLUTION_PROBE
read_track_turn:
    mov si,read_track_command
    mov ax,9 * 512 - 1
    jmp read_turn

read_turn_marker:
    mov si,read_marker
    mov ax,512 - 1
read_turn:
    push ax
    mov al,6
    out 0ah,al
    out 0ch,al
    mov al,46h
    out 0bh,al
    xor al,al
    out 04h,al
    mov al,12h
    out 04h,al
    pop ax
    out 05h,al
    mov al,ah
    out 05h,al
    mov al,2
    out 0ah,al
    mov cx,9
    call command_and_irq
    call read_result
%ifdef TC_WEAK_BITS
    cmp word [result+1],02020h * TC_WEAK_BITS
%else
    cmp word [result],DRIVE_HEAD
%endif
    jne finish
    in al,08h
    test al,4
    jz finish
    ret
%endif

%ifdef TC_WEAK_BITS
check_tc_weak:
    call bp
    xor si,si
.save:
    mov ax,[es:bx+si]
    mov [weak_sample+si],ax
    add si,2
    cmp si,16
    jb .save
%ifdef READBACK
    cmp word [es:bx+16],5454h
%else
    cmp word [es:bx+16],0
%endif
    jne finish
    call bp
    xor si,si
    xor dx,dx
.compare:
    mov ax,[es:bx+si]
    xor ax,[weak_sample+si]
    or dx,ax
    add si,2
    cmp si,16
    jb .compare
%ifdef READBACK
    cmp word [es:bx+16],5454h
%else
    cmp word [es:bx+16],0
%endif
    jne finish
    test dx,dx
%if TC_WEAK_BITS
    jz finish
%else
    jnz finish
%endif
    ret

weak_sample times 16 db 0
%endif

%ifdef DUPLICATE_TEST
test_duplicate:
    mov byte [stage],14
    cld
    mov ax,6000h
    mov es,ax
%ifdef DUPLICATE_WRITE
    call find_duplicate_pair
    call read_duplicate_sector
    mov al,65h
    call check_duplicate_fill
    call read_duplicate_sector
    mov al,66h
    call check_duplicate_fill

    mov byte [stage],15
    call find_duplicate_pair
%if UNIT = 1
    call read_duplicate_sector
%endif
    mov di,1200h
    mov cx,512
    mov al,0a7h
    rep stosb
    mov al,4ah
    mov si,write_duplicate
    call transfer_duplicate
%endif
    mov byte [stage],16
    mov bp,2
.check_revolution:
    call find_duplicate_pair
    call read_duplicate_sector
%if UNIT = 0
    mov al,0a7h
%else
    mov al,65h
%endif
    call check_duplicate_fill
    call read_duplicate_sector
%if UNIT = 0
    mov al,66h
%else
    mov al,0a7h
%endif
    call check_duplicate_fill
    dec bp
    jnz .check_revolution
    mov byte [stage],0
    jmp finish

find_duplicate_pair:
    ; Sector 3 immediately precedes the first occurrence of ID 6.
    mov bx,9
.next_id:
    call read_id
    cmp byte [result+5],3
    je .found
    dec bx
    jnz .next_id
    jmp finish
.found:
    ret

read_duplicate_sector:
    mov al,46h
    mov si,read_duplicate
transfer_duplicate:
    mov ah,al
    mov al,6
    out 0ah,al
    out 0ch,al
    mov al,ah
    out 0bh,al
    xor al,al
    out 04h,al
    mov al,12h
    out 04h,al
    mov al,0ffh
    out 05h,al
    mov al,1
    out 05h,al
    mov al,6
    out 81h,al
    mov al,2
    out 0ah,al
    mov cx,9
    call command_and_irq
    call read_result
    cmp word [result],DRIVE_HEAD
    jne finish
    cmp byte [result+2],0
    jne finish
    cmp word [result+3],0103h
    jne finish
    cmp word [result+5],0201h
    jne finish
    in al,08h
    test al,4
    jz finish
    ret

check_duplicate_fill:
    mov di,1200h
    mov cx,512
    repe scasb
    jne finish
    ret

read_duplicate db 46h,DRIVE_HEAD,2,1,6,2,6,2ah,0ffh
write_duplicate db 45h,DRIVE_HEAD,2,1,6,2,6,2ah,0ffh
%endif

reset_and_seek:
    mov byte [interrupts],0
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
    ret

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
%ifdef SCP_FORMAT
    cmp byte [result+5],11
%else
    cmp byte [result+5],SECTORS
%endif
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
    call send_command
    jmp wait_irq

send_command:
    mov byte [interrupts],0
.next:
    lodsb
    call send
    loop .next
    ret

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
%ifdef BIOS_FORMAT
    inc byte [cs:interrupts]
    jmp far [cs:bios_irq6]
bios_irq6 dw 0,0
%else
    push ax
    inc byte [cs:interrupts]
    mov al,20h
    out 20h,al
    pop ax
    iret
%endif

recalibrate db 7,UNIT
seek db 0fh,DRIVE_HEAD,2
id db 4ah,DRIVE_HEAD
%ifdef FORMAT_TEST
format_command db 4dh,DRIVE_HEAD,2,SECTORS,54h,0e5h
format_ids:
%ifdef SCP_FORMAT
%ifdef FORMAT_PARTIAL
    db 2,1,10,2, 2,1,11,2, 2,1,12,2
    times (SECTORS - 3) * 4 db 0
%else
    db 2,1,6,2, 2,1,3,2, 2,1,10,2, 2,1,4,2
    db 2,1,1,2, 2,1,2,2, 2,1,5,2, 2,1,7,2
%ifdef FORMAT_INVALID
    db 2,1,11,8
%else
    db 2,1,11,2
%endif
%endif
%else
%assign sector 1
%rep SECTORS - 1
    db 2,1,sector,2
%assign sector sector + 1
%endrep
%ifdef FORMAT_INVALID
    db 2,1,SECTORS - 1,2
%else
    db 2,1,SECTORS,2
%endif
%endif
%endif
%ifdef READBACK
read_data db 46h,DRIVE_HEAD,2,1,5,2,6,2ah,0ffh
%elifdef SCP_FORMAT_READBACK
read_data db 46h,DRIVE_HEAD,2,1,10,2,11,2ah,0ffh
%ifdef FORMAT_PARTIAL
read_survivors db 46h,DRIVE_HEAD,2,1,6,2,7,2ah,0ffh
%endif
%else
read_data db 46h,DRIVE_HEAD,2,1,3,2,4,2ah,0ffh
%endif
%ifdef WRITE_TEST
write_data db 45h,DRIVE_HEAD,2,1,5,2,6,2ah,0ffh
%endif
%ifdef TC_STEPPED
seek_skipped db 0fh,DRIVE_HEAD,1
%endif
file db 'FDC.OUT',0
stage db 0
saved_imr db 0
interrupts db 0
result times 7 db 0
%ifdef REORDERED
%ifdef SCP_FORMAT_READBACK
%ifdef FORMAT_PARTIAL
next_id db 0,0,5,0,6,7,8,9,10,11,3
%else
next_id db 2,5,10,1,7,3,11,0,0,4,6
%endif
%else
next_id db 2,4,5,3,6,7,8,9,1
%endif
%endif
%ifdef REVOLUTION_PROBE
read_marker db 46h,DRIVE_HEAD,2,1,6,2,6,2ah,0ffh
read_track_command db 42h,DRIVE_HEAD,2,1,1,2,9,2ah,0ffh
%endif
