; SPDX-License-Identifier: GPL-2.0-or-later

bits 16
org 100h

    cld
    xor ax,ax
    mov dx,FAT_UNIT
    int 13h
    jc fail
    mov dx,3f2h
    mov al,0ch | FAT_UNIT | (10h << FAT_UNIT)
    out dx,al

    call check_open_data
    inc byte [stage]
    call fat_check_metadata
    mov byte [stage],0
fail:
    mov dx,report
    xor cx,cx
    mov ah,3ch
    int 21h
    jc .exit
    mov bx,ax
    mov dx,stage
    mov cx,1
    mov ah,40h
    int 21h
    call fat_close
.exit:
    mov ax,4c00h
    int 21h

check_open_data:
    mov dx,data_filename
    mov ax,3d00h
    int 21h
    jc fail
    mov [data_handle],ax
    mov byte [expected_data],31h
    call read_data_byte
    mov byte [cache_direct],0
    mov al,71h
    call replace_data
    call read_data_byte
    mov byte [cache_direct],1
    mov al,72h
    call replace_data
    call read_data_byte
%ifndef FAT_CACHE_READ_ONLY
    ; DOS renames move the directory entry while the handle remains open.
    push ds
    pop es
    mov dx,data_filename
    mov di,renamed_filename
    mov ah,56h
    int 21h
    jc fail
    call read_data_byte
    push ds
    pop es
    mov dx,renamed_filename
    mov di,data_filename
    mov ah,56h
    int 21h
    jc fail
    call read_data_byte
%endif
    mov bx,[data_handle]
    jmp fat_close

replace_data:
%ifndef FAT_CACHE_READ_ONLY
    mov [expected_data],al
%endif
    push ds
    pop es
    mov di,[buffer]
    mov cx,512
    rep stosb
    mov cx,7
    jmp cache_media_write

read_data_byte:
    mov bx,[data_handle]
    mov dx,cache_data
    mov cx,1
    mov ah,3fh
    int 21h
    jc fail
    cmp ax,1
    jne fail
    mov al,[expected_data]
    cmp [cache_data],al
    jne fail
    ret

fat_close:
    mov ah,3eh
    int 21h
    ret

fat_int13:
    push ds
    pop es
    int 13h
    ret

dma:
    push ax
    mov al,6
    out 0ah,al
    out 0ch,al
    pop ax
    out 0bh,al
    mov ax,ds
    mov dx,ax
    shr dx,12
    shl ax,4
    add ax,[buffer]
    adc dx,0
    out 04h,al
    mov al,ah
    out 04h,al
    mov al,dl
    out 81h,al
    mov ax,cx
    dec ax
    out 05h,al
    mov al,ah
    out 05h,al
    in al,08h
    mov al,2
    out 0ah,al
    ret

send_command:
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
    inc dx
    push ds
    pop es
    mov di,result
    mov cx,7
.result:
    in al,dx
    stosb
    loop .result
    ret

normal:
    test byte [result],0c0h
    jnz fail
    cmp word [result+1],0
    jne fail
    ret

report db 'FAT.OUT',0
stage db 1
data_filename db 'A'+FAT_UNIT,':\CHECK.DAT',0
renamed_filename db 'A'+FAT_UNIT,':\CACHE.DAT',0
expected_data db 0
data_handle dw 0
result times 7 db 0
buffer dw scratch
scratch times 512 db 0

fat_check_metadata:
    ; Keep both handles open across each chain rewrite. The write handle
    ; must resolve its current sector again without a preceding DOS read.
    mov byte [cache_direct],0
    mov byte [cache_current],44h
    mov byte [cache_next],55h
    mov byte [cache_link],5
    mov byte [cache_new_name],'X'
    mov byte [cache_old_name],'B'
    mov byte [cache_write_byte],75h
    call cache_round
    mov byte [cache_direct],1
    mov byte [cache_current],75h
    mov byte [cache_next],66h
    mov byte [cache_link],6
    mov byte [cache_new_name],'Y'
%ifndef FAT_CACHE_READ_ONLY
    mov byte [cache_old_name],'X'
%endif
    mov byte [cache_write_byte],76h
    call cache_round
    ret

cache_round:
    mov dx,cache_old_name_path
    call cache_find
    jc fail
%ifdef FAT_CACHE_READ_ONLY
    mov ax,3d00h
%else
    mov ax,3d02h
%endif
    call cache_open_at_tail
    mov [cache_writer],bx
    mov ax,3d00h
    call cache_open_at_tail
    mov [cache_reader],bx

    mov ax,0201h
    mov cx,6                      ; root directory, cylinder 0 head 0
    call cache_bios_io
    jc fail
    mov di,[buffer]
    mov al,[cache_new_name]
    mov [di+64],al
    inc word [di+32+28]            ; extend CHAIN.DAT by one byte
    mov cl,6
    call cache_media_write

    mov ax,0201h
    mov cx,2                      ; first FAT sector
    call cache_bios_io
    jc fail
    mov di,[buffer]
    mov al,[cache_link]
    shl al,4
    or al,0fh                     ; preserve the adjacent cluster 2 nibble
    mov [di+4],al
    mov byte [di+5],0
    mov cl,2
    call cache_media_write
    mov cl,4                      ; mirrored FAT
    call cache_media_write

    mov bx,[cache_reader]
    mov dx,cache_data
    mov cx,4
    mov ah,3fh
    int 21h
    jc fail
%ifdef FAT_CACHE_READ_ONLY
    cmp ax,2
%else
    mov cx,3
    cmp byte [cache_direct],0
    je .check_length
    inc cx
.check_length:
    cmp ax,cx
%endif
    jne fail
%ifdef FAT_CACHE_READ_ONLY
    mov al,44h
%else
    mov al,[cache_next]
%endif
    cmp [cache_data],al
    jne fail
    cmp [cache_data+1],al
    jne fail
    call fat_close

    mov bx,[cache_writer]
%ifndef FAT_CACHE_READ_ONLY
    mov dx,cache_write_byte
    mov cx,1
    mov ah,40h
    int 21h
    jc fail
    cmp ax,1
    jne fail
%endif
    call fat_close
    mov dx,cache_old_name_path
    call cache_find
%ifdef FAT_CACHE_READ_ONLY
    jc fail
    mov dx,cache_new_name_path
    call cache_find
    jnc fail
%else
    jnc fail
    mov dx,cache_new_name_path
    call cache_find
    jc fail
%endif

    ; Read the replacement cluster via DOS after closing both handles.
    mov ax,3d00h
    call cache_open_at_tail
    mov dx,cache_data
    mov cx,4
    mov ah,3fh
    int 21h
    jc fail
%ifdef FAT_CACHE_READ_ONLY
    cmp ax,2
%else
    mov cx,3
    cmp byte [cache_direct],0
    je .check_written_length
    inc cx
.check_written_length:
    cmp ax,cx
%endif
    jne fail
%ifdef FAT_CACHE_READ_ONLY
    cmp word [cache_data],4444h
    jne fail
%else
    mov al,[cache_write_byte]
    cmp [cache_data],al
    jne fail
    mov al,[cache_next]
    cmp [cache_data+1],al
    jne fail
    ; The prior cluster's first byte must survive the DOS write.
    mov ax,0201h
    mov cx,2                      ; cluster 4, head 1, sector 2
    cmp byte [cache_direct],0
    je .check_old_cluster
    mov cl,4                      ; cluster 5, head 1, sector 4
.check_old_cluster:
    push bx
    mov bx,[buffer]
    mov dx,0100h | FAT_UNIT
    call fat_int13
    pop bx
    jc fail
    mov di,[buffer]
    mov al,[cache_current]
    cmp [di],al
    jne fail
%endif
    jmp fat_close

cache_find:
    xor cx,cx
    mov ah,4eh
    int 21h
    ret

cache_open_at_tail:
    mov dx,cache_filename
    int 21h
    jc fail
    mov bx,ax
    mov ax,4200h
    xor cx,cx
    mov dx,1024
    int 21h
    jc fail
    cmp ax,1024
    jne fail
    ret

cache_bios_io:
    mov bx,[buffer]
    mov dx,FAT_UNIT
    jmp fat_int13

cache_media_write:
    xor ch,ch
    cmp byte [cache_direct],0
    jne .direct
    mov ax,0301h
    call cache_bios_io
%ifdef FAT_CACHE_READ_ONLY
    jnc fail
    cmp ah,03h
    jne fail
%else
    jc fail
%endif
    ret
.direct:
    mov [cache_fdc_write+4],cl
    mov [cache_fdc_write+6],cl
    mov cx,512
    mov al,4ah
    call dma
    mov si,cache_fdc_write
    mov cx,9
    call send_command
%ifdef FAT_CACHE_READ_ONLY
    cmp byte [result+1],2
    jne fail
    ret
%else
    jmp normal
%endif

cache_filename db 'A'+FAT_UNIT,':\CHAIN.DAT',0
cache_old_name_path db 'A'+FAT_UNIT,':\'
cache_old_name db 'B','EFORE.DAT',0
cache_new_name_path db 'A'+FAT_UNIT,':\'
cache_new_name db 'X','EFORE.DAT',0
cache_direct db 0
cache_current db 0
cache_next db 0
cache_link db 0
cache_write_byte db 0
cache_writer dw 0
cache_reader dw 0
cache_data dd 0
cache_fdc_write db 45h,FAT_UNIT,0,0,0,2,0,42,255
