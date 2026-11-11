; SPDX-License-Identifier: GPL-2.0-or-later

bits 16
org 100h

%ifndef CYLINDER
%define CYLINDER 7
%endif

    push ds
    pop es
    cld
%ifdef FIXED_DISK
    ; Check two adjacent 256-byte sectors with distinct payloads.
    mov byte [stage],1
    mov ax,0202h
    mov cx,1
    mov dx,80h
    mov bx,buffer
    int 13h
    jc report
    mov di,buffer
    mov cx,256
    mov al,31h
    repe scasb
    jne report
    mov cx,256
    mov al,32h
    repe scasb
    jne report
    cmp byte [di],0a5h
    jne report

    mov byte [stage],2
    mov di,buffer
    mov cx,512
    mov al,77h
    rep stosb
    mov ax,0302h
    mov cx,1
    int 13h
    jc report
    ; Read the written sectors again to flush the image and verify the data.
    mov ax,0202h
    mov cx,1
    int 13h
    jc report
    mov di,buffer
    mov cx,512
    mov al,77h
    repe scasb
    jne report
%else
    mov dx,parameters
    mov ax,251eh
    int 21h
%ifndef OVERLAY_REPLAY
    mov byte [stage],1
    mov ax,0502h
    mov bx,ids
    mov cx,(CYLINDER << 8) | 1
    xor dx,dx
    int 13h
    jc report
%endif

%ifdef OVERLAY_WRITE
    mov byte [stage],4
    mov di,buffer
    mov cx,SECTOR_BYTES
    mov al,31h
    rep stosb
    mov cx,SECTOR_BYTES
    mov al,72h
    rep stosb
    in al,08h
    mov ax,0302h
    mov bx,buffer
    mov cx,(CYLINDER << 8) | 1
    xor dx,dx
    int 13h
    jc report
    cmp ax,2
    jne report
%ifndef SECTOR_PATH
    in al,08h
    test al,4
    jz report
%endif
%endif

    mov byte [stage],2
    mov di,buffer
    mov cx,2 * SECTOR_BYTES
    mov al,0a5h
    rep stosb
    mov ax,0202h
    mov bx,buffer
    mov cx,(CYLINDER << 8) | 1
    xor dx,dx
    int 13h
    jc report
    cmp ax,2
    jne report
    mov di,buffer
%if %isdef(OVERLAY_WRITE) || %isdef(OVERLAY_REPLAY)
    mov cx,SECTOR_BYTES
    mov al,31h
    repe scasb
    jne report
    mov cx,SECTOR_BYTES
    mov al,72h
%else
    mov cx,2 * SECTOR_BYTES
    mov al,67h
%endif
    repe scasb
    jne report
    cmp byte [di],0a5h
    jne report

    mov byte [stage],3
    mov di,buffer
    mov cx,2 * SECTOR_BYTES
    mov al,0a5h
    rep stosb
    mov ax,0402h
    mov cx,(CYLINDER << 8) | 1
    int 13h
    jc report
    cmp ax,2
    jne report
    mov di,buffer
    mov cx,2 * SECTOR_BYTES + 1
    mov al,0a5h
    repe scasb
    jne report
%ifdef OVERLAY_REPLAY
    ; Replay older logical and physical 512-byte records after upgrading.
    mov byte [stage],5
    mov byte [parameters+3],2
    mov cx,9
    mov bp,59h
    call check_legacy
    mov cx,1
    mov bp,73h
    call check_legacy
%endif
%endif
    mov byte [stage],0

report:
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

%ifdef OVERLAY_REPLAY
check_legacy:
    mov ax,0201h
    mov bx,buffer
    xor dx,dx
    int 13h
    jc report
    cmp ax,1
    jne report
    mov ax,bp
    mov di,buffer
    mov cx,512
    repe scasb
    jne report
    ret
%endif

%ifndef FIXED_DISK
parameters db 0dfh,2,25h,SIZE_CODE,9,2ah,0ffh,54h,67h,15,8
ids db CYLINDER,0,1,SIZE_CODE, CYLINDER,0,2,SIZE_CODE
%endif
filename db 'SIZE.OUT',0
stage db 0
buffer times 2049 db 0a5h
