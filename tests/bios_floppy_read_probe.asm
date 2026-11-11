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
%ifndef PCJR_IRQ_TEST
    or al,40h
    out 21h,al
%endif
    cld
%ifdef READ_TRACK_TEST
    jmp read_track_test
%endif
%ifdef IRQ_RESET_TEST
    jmp irq_reset_test
%endif
%ifdef CROSS_TRACK
    jmp cross_track
%endif
%ifdef VERIFY_TEST
%ifdef TRACK_IMAGE
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
    ; Cross from head 0 to head 1, then cross a cylinder boundary.
    mov byte [verify_head],0
    mov cx,0209h
    mov dx,0002h
    call check_verify
    inc byte [stage]
    mov byte [verify_head],1
    mov cx,0209h
    mov dx,0002h
    call check_verify
    inc byte [stage]
    ; The final sector verifies, but the next cylinder is outside the image.
    mov cx,2709h
    mov dx,0401h
    call check_verify
    inc byte [stage]
    mov dx,0400h
    mov cx,2801h
    call check_verify
    inc byte [stage]
    mov cx,020ah
    mov dx,0400h
    call check_verify
    inc byte [stage]
    mov cx,0200h
    mov dx,0400h
    call check_verify
    inc byte [stage]
    mov byte [verify_head],2
    mov cx,0201h
    mov dx,0400h
    call check_verify
    inc byte [stage]
    mov byte [verify_head],0
    mov byte [verify_count],0
    mov dx,0100h
    call check_verify
%endif
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
    mov ah,04h
    mov al,[verify_count]
    mov dh,[verify_head]
    mov dl,UNIT
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
%ifdef TRACK_IMAGE
    cmp byte [interrupts],1
    jne report
%else
    cmp byte [interrupts],0
    jne report
    mov al,0ah
    out 20h,al
    in al,20h
    test al,40h
    jnz report
%endif
    push es
    xor ax,ax
    mov es,ax
    cmp [es:0441h],dh
    pop es
    jne report
    mov ah,01h
    mov dl,UNIT
    int 13h
    pushf
    pop cx
    xor cx,si
    test cx,1
    jnz report
    cmp ah,dh
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

%ifdef CROSS_TRACK
; Each row specifies the BIOS request, result, IRQ count, and expected buffer.
%macro cross_case 10
    mov ax,%1
    mov cx,%2
    mov dx,(%3 << 8) | UNIT
    mov bx,%4
    call cross_transfer
    dw %5
    db %6,%7,%8,%9,%10
%endmacro

cross_track:
%ifdef CROSS_WRITE
    cross_case 0302h,0109h,0,1100h,0002h,2,0a5h,0a5h,0b6h,0c7h
    cross_case 0302h,0109h,1,1100h,0002h,2,0a5h,0a5h,0b6h,0c7h
    cross_case 0303h,0209h,1,1100h,0402h,2,0a5h,0a5h,0b6h,0c7h
    cross_case 0302h,0109h,0,0fb01h,0900h,0,0cch,0cch,0cch,0cch
%elifdef CROSS_READBACK
    cross_case 0202h,0109h,0,1100h,0002h,2,0cch,0a5h,0b6h,0cch
    cross_case 0202h,0109h,1,1100h,0002h,2,0cch,0a5h,0b6h,0cch
    cross_case 0203h,0209h,1,1100h,0402h,2,0cch,0a5h,0b6h,0cch
%else
    cross_case 0202h,0109h,0,0fb00h,0002h,2,0cch,19h,21h,0cch
    cross_case 0202h,0109h,1,1100h,0002h,2,0cch,29h,31h,0cch
    cross_case 0203h,0209h,1,1100h,0402h,2,0cch,39h,41h,0cch
    cross_case 0202h,0109h,0,0fb01h,0900h,0,0cch,0cch,0cch,0cch
    cross_case 0402h,0109h,0,0fb01h,0002h,2,0cch,0cch,0cch,0cch
    cross_case 0402h,0109h,1,1100h,0002h,2,0cch,0cch,0cch,0cch
    cross_case 0403h,0209h,1,1100h,0402h,2,0cch,0cch,0cch,0cch
    cross_case 0402h,2709h,1,1100h,0401h,1,0cch,0cch,0cch,0cch
%endif
    mov byte [stage],0
    jmp report

cross_transfer:
    pop si
    push ax
    push cx
    mov ah,al
    mov cx,6010h
    mov es,cx
    mov di,bx
    mov cx,1024
    cmp ah,3
    jne .fill_buffer
    mov cx,1536
.fill_buffer:
    mov al,[si+3]
    rep stosb
%ifdef CROSS_WRITE
    mov di,bx
    add di,512
    mov cx,512
    mov al,[si+5]
    rep stosb
    cmp ah,3
    jne .buffer_ready
    mov cx,512
    mov al,[si+6]
    rep stosb
.buffer_ready:
%endif
    pop cx
    pop ax
    mov byte [interrupts],0
    push si
    push bx
    push ax
    int 13h
    pop bp
    pop di
    pop si
    pushf
    pop dx
    cmp ax,[si]
    jne report
    test ah,ah
    jz .success
    test dl,1
    jz report
    jmp .check_irq
.success:
    test dl,1
    jnz report
.check_irq:
    mov al,[interrupts]
    cmp al,[si+2]
    jne report
    mov bx,4
.check_sector:
    mov al,[si+bx]
    mov cx,512
    repe scasb
    jne report
    inc bx
    cmp bx,6
    jb .check_sector
    je .check_third
    jmp .check_mask
.check_third:
    and bp,0ffh
    cmp bp,3
    je .check_sector
.check_mask:
    in al,21h
    test al,40h
    jz report
    inc byte [stage]
    add si,7
    jmp si
%endif

%ifdef IRQ_RESET_TEST
irq_reset_test:
%ifndef PCJR_IRQ_TEST
    ; Leave the controller waiting for the parameters of a Seek command.
    mov dx,3f2h
    mov al,0ch | UNIT | (10h << UNIT)
    out dx,al
    mov dx,3f5h
    mov al,0fh
    out dx,al
%endif
    mov byte [interrupts],0
    xor ax,ax
    mov dx,UNIT
    int 13h
    jc report
%ifdef PCJR_IRQ_TEST
    cmp byte [interrupts],0
    jne report
%else
    cmp byte [interrupts],1
    jne report
    cmp byte [pic_irq6],1
    jne report
    mov ax,40h
    mov es,ax
    test byte [es:3eh],80h
    jz report
    in al,21h
    test al,40h
    jz report
    ; The BIOS must drain reset status and leave the controller ready.
    mov dx,3f4h
    in al,dx
    cmp al,80h
    jne report
    mov dx,3f5h
    mov al,08h
    out dx,al
    in al,dx
    cmp al,80h
    jne report
    inc byte [stage]
    in al,21h
    and al,0bfh
    out 21h,al
    mov dx,3f2h
    mov al,0
    out dx,al
    mov al,0ch | UNIT | (10h << UNIT)
    out dx,al
.wait_irq:
    cmp byte [interrupts],2
    je .check_edge
    hlt
    jmp .wait_irq
.check_edge:
    in al,21h
    or al,40h
    out 21h,al
    mov al,0ch | UNIT | (10h << UNIT)
    out dx,al
    mov al,0ah
    out 20h,al
    in al,20h
    test al,40h
    jnz report
    mov dx,3f5h
    mov cx,4
.drain:
    mov al,08h
    out dx,al
    in al,dx
    in al,dx
    loop .drain
%endif
    inc byte [stage]
    mov ax,6000h
    mov es,ax
    mov bx,1100h
    mov ax,0201h
    mov cx,0203h
    mov dx,0100h | UNIT
    int 13h
    jc report
    cmp byte [es:1100h],53h
    jne report
%ifdef KEYBOARD_TEST
    inc byte [stage]
    xor ax,ax
    int 16h
    cmp al,'k'
    jne report
%ifdef PCJR_IRQ_TEST
    cmp byte [interrupts],0
    je report
%endif
%endif
    mov byte [stage],0
    jmp report
%endif

%ifdef READ_TRACK_TEST
read_track_test:
    ; BIOS Read Sectors must reject the 256-byte ID and leave the buffer alone.
    mov ax,6000h
    mov es,ax
    mov di,10ffh
    mov cx,514
    mov al,0a5h
    rep stosb
    mov bx,1100h
    mov ax,0201h
    mov cx,0201h
    xor dx,dx
    int 13h
    jnc report
    cmp ah,04h
    jne report
    mov di,10ffh
    mov cx,514
    mov al,0a5h
    repe scasb
    jne report

    inc byte [stage]
    ; Program DMA channel 2 for exactly one 512-byte field at 61100h.
    mov al,6
    out 0ah,al
    out 0ch,al
    mov al,46h
    out 0bh,al
    xor al,al
    out 04h,al
    mov al,11h
    out 04h,al
    mov al,0ffh
    out 05h,al
    mov al,1
    out 05h,al
    mov al,6
    out 81h,al
    in al,08h
    mov al,2
    out 0ah,al

    mov ax,40h
    mov es,ax
    and byte [es:3eh],7fh
    mov byte [interrupts],0
%ifndef PCJR_IRQ_TEST
    in al,21h
    and al,0bfh
    out 21h,al
%endif
    mov si,read_track_command
    mov cx,9
    mov dx,3f5h
.send:
    lodsb
    out dx,al
    loop .send
    mov dx,3f4h
.wait:
    in al,dx
    and al,0d0h
    cmp al,0d0h
    jne .wait
    test byte [es:3eh],80h
    jz .wait
%ifdef PCJR_IRQ_TEST
    cmp byte [interrupts],0
%else
    cmp byte [interrupts],1
%endif
    jne report

    inc byte [stage]
    mov dx,3f5h
    mov si,read_track_result
    mov cx,7
.result:
    in al,dx
    cmp al,[si]
    jne report
    inc si
    loop .result
    mov dx,3f4h
    in al,dx
    cmp al,80h
    jne report
    in al,08h
    test al,04h
    jz report

    inc byte [stage]
    mov ax,6000h
    mov es,ax
    cmp byte [es:10ffh],0a5h
    jne report
    cmp byte [es:1300h],0a5h
    jne report
    mov di,1100h
    mov cx,511
    xor al,al
.payload:
    scasb
    jne report
    inc al
    loop .payload
    cmp byte [es:di],0f7h
    jne report

    mov byte [stage],10h
    call check_read_track_dma_mask

%ifndef PCJR_IRQ_TEST
    inc byte [stage]
    int 12h
    cmp ax,96
    jne report
    mov ax,9
    int 10h
    mov ax,0b800h
    mov es,ax
    mov ax,1800h
    mov ds,ax
    xor bx,bx
    mov cx,4
    mov al,35h
.alias:
    mov [bx],al
    cmp [es:bx],al
    jne .restore_ds
    inc al
    mov [es:bx],al
    cmp [bx],al
    jne .restore_ds
    inc al
    add bx,2000h
    loop .alias
    push cs
    pop ds
%endif
    mov byte [stage],0
    jmp report

%ifndef PCJR_IRQ_TEST
.restore_ds:
    push cs
    pop ds
    jmp report
%endif

read_track_command db 42h,0,2,0,1,2,9,2ah,0ffh
; Terminal count ends the command normally. ST1 retains ID CRC and size mismatch.
read_track_result db 00h,24h,00h,02h,00h,01h,01h

check_read_track_dma_mask:
    ; Position at C2/H1, where the third field has a bad data CRC.
    mov bx,1100h
    mov ax,0201h
    mov cx,0201h
    mov dx,0100h
    int 13h
    jc report

    ; Request five 512-byte fields. Mask DMA after the third field.
    mov al,6
    out 0ah,al
    out 0ch,al
    mov al,46h
    out 0bh,al
    xor al,al
    out 04h,al
    mov al,11h
    out 04h,al
    mov al,0ffh
    out 05h,al
    mov al,9
    out 05h,al
    mov al,6
    out 81h,al
    mov al,2
    out 0ah,al
    mov si,read_crc_track_command
    mov cx,9
    mov dx,3f5h
.send:
    lodsb
    out dx,al
    loop .send
.wait_count:
    out 0ch,al
    in al,05h
    in al,05h
    cmp al,3
    jne .wait_count
    mov al,6
    out 0ah,al
    inc byte [stage]

    mov dx,3f4h
.wait_result:
    in al,dx
    and al,0d0h
    cmp al,0d0h
    jne .wait_result
    inc dx
    mov si,read_crc_track_result
    mov cx,7
.result:
    in al,dx
    cmp al,[si]
    jne report
    inc si
    loop .result
    ret

read_crc_track_command db 42h,4,2,1,1,2,9,2ah,0ffh
read_crc_track_result db 44h,30h,20h,2,1,3,2
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
%ifdef IRQ_RESET_TEST
%ifndef PCJR_IRQ_TEST
    push ax
    mov al,0bh
    out 20h,al
    in al,20h
    test al,40h
    jz .restore
    inc byte [cs:pic_irq6]
.restore:
    mov al,0ah
    out 20h,al
    pop ax
%endif
%endif
    inc byte [cs:interrupts]
    jmp far [cs:old_irq6]

old_irq6 dd 0
saved_mask db 0
interrupts db 0
pic_irq6 db 0
verify_head db 1
verify_count db 2
sector_count db 0
stage db 1
filename db 'BIOS.OUT',0
