; SPDX-License-Identifier: GPL-2.0-or-later

bits 16
org 100h

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
    mov byte [stage],1
    mov ax,0502h
    mov bx,ids
    mov cx,0701h
    xor dx,dx
    int 13h
    jc report

    mov byte [stage],2
    mov ax,0202h
    mov bx,buffer
    int 13h
    jc report
    cmp ax,2
    jne report
    mov di,buffer
    mov cx,2 * SECTOR_BYTES
    mov al,67h
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
    mov cx,0701h
    int 13h
    jc report
    cmp ax,2
    jne report
    mov di,buffer
    mov cx,2 * SECTOR_BYTES + 1
    mov al,0a5h
    repe scasb
    jne report
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

%ifndef FIXED_DISK
parameters db 0dfh,2,25h,SIZE_CODE,9,2ah,0ffh,54h,67h,15,8
ids db 7,0,1,SIZE_CODE, 7,0,2,SIZE_CODE
%endif
filename db 'SIZE.OUT',0
stage db 0
buffer times 2049 db 0a5h
