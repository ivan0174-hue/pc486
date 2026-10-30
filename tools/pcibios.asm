; pc486 - PCI BIOS extension ROM (INT 1Ah, AH=B1h, PCI BIOS 2.10)
; Loaded at C8000h; the AMI BIOS calls the init entry during POST.
; Uses configuration mechanism #1 (CF8h/CFCh). Bus 0 only.
        bits 16
        cpu 386
        org 0

        db 0x55, 0xaa
        db 2                    ; size in 512 byte blocks
        jmp near init
        times 0x18 - ($ - $$) db 0
        dw 0                    ; no PCI data structure (this is not a card ROM)
        dw 0

init:
        push ds
        push ax
        xor ax, ax
        mov ds, ax
        ; other INT 1Ah functions go to the standard BIOS entry point
        mov word [0x1a * 4], int1a
        mov word [0x1a * 4 + 2], cs
        pop ax
        pop ds
        retf

; ---------------------------------------------------------------------
int1a:
        cmp ah, 0xb1
        je pci
        jmp 0xf000:0xfe6e       ; IBM-compatible INT 1Ah entry of the BIOS

pci:
        cmp al, 0x01
        je fn_present
        cmp al, 0x02
        je fn_find_dev
        cmp al, 0x03
        je fn_find_class
        cmp al, 0x08
        je fn_rd8
        cmp al, 0x09
        je fn_rd16
        cmp al, 0x0a
        je fn_rd32
        cmp al, 0x0b
        je fn_wr8
        cmp al, 0x0c
        je fn_wr16
        cmp al, 0x0d
        je fn_wr32
        mov ah, 0x81            ; function not supported
        jmp err

fn_present:
        mov edx, 0x20494350     ; "PCI "
        mov ax, 0x0001          ; AH = 0 (present), AL = mechanism #1
        mov bx, 0x0210          ; version 2.10
        xor cl, cl              ; last bus
        xor edi, edi            ; no protected mode entry
        jmp ok

; CX = device id, DX = vendor id, SI = index -> BH = bus, BL = devfn
fn_find_dev:
        push eax
        push ecx
        push esi
        mov bx, 0               ; devfn
.loop:  mov eax, 0x80000000     ; bus 0, devfn in bits 15:8, register 0
        mov ah, bl
        push dx
        mov dx, 0xcf8
        out dx, eax
        mov dx, 0xcfc
        in eax, dx
        pop dx
        cmp ax, dx
        jne .next
        shr eax, 16
        cmp ax, cx
        jne .next
        or si, si
        jz .found
        dec si
.next:  add bl, 8
        jnz .loop
        pop esi
        pop ecx
        pop eax
        mov ah, 0x86            ; device not found
        jmp err
.found: xor bh, bh
        pop esi
        pop ecx
        pop eax
        mov ah, 0
        jmp ok

; ECX = class code (bits 23:0), SI = index
fn_find_class:
        push eax
        push esi
        push edx
        mov bx, 0
.loop:  mov eax, 0x80000008
        mov ah, bl
        mov dx, 0xcf8
        out dx, eax
        mov dx, 0xcfc
        in eax, dx
        shr eax, 8
        push ecx
        and ecx, 0x00ffffff
        cmp eax, ecx
        pop ecx
        jne .next
        or si, si
        jz .found
        dec si
.next:  add bl, 8
        jnz .loop
        pop edx
        pop esi
        pop eax
        mov ah, 0x86
        jmp err
.found: xor bh, bh
        pop edx
        pop esi
        pop eax
        mov ah, 0
        jmp ok

; select BH:BL register DI (dword aligned), returns DX = data port
select:
        push eax
        push ebx
        movzx eax, bh           ; bus
        shl eax, 16
        mov ah, bl              ; devfn
        mov bx, di
        and bl, 0xfc
        mov al, bl              ; register
        or eax, 0x80000000
        pop ebx
        mov dx, 0xcf8
        out dx, eax
        pop eax
        mov dx, di
        and dx, 3
        add dx, 0xcfc
        ret

fn_rd8: push dx
        call select
        in al, dx
        mov cl, al
        pop dx
        mov ah, 0
        jmp ok
fn_rd16:
        push dx
        call select
        in ax, dx
        mov cx, ax
        pop dx
        mov ah, 0
        jmp ok
fn_rd32:
        push dx
        call select
        in eax, dx
        mov ecx, eax
        pop dx
        mov ah, 0
        jmp ok
fn_wr8: push dx
        push ax
        call select
        mov al, cl
        out dx, al
        pop ax
        pop dx
        mov ah, 0
        jmp ok
fn_wr16:
        push dx
        push ax
        call select
        mov ax, cx
        out dx, ax
        pop ax
        pop dx
        mov ah, 0
        jmp ok
fn_wr32:
        push dx
        push eax
        call select
        mov eax, ecx
        out dx, eax
        pop eax
        pop dx
        mov ah, 0
        jmp ok

ok:     push bp
        mov bp, sp
        and byte [bp + 6], 0xfe ; clear CF in the saved flags
        pop bp
        iret
err:    push bp
        mov bp, sp
        or byte [bp + 6], 0x01  ; set CF
        pop bp
        iret

        times 1023 - ($ - $$) db 0
        db 0                    ; checksum (patched when loaded)
