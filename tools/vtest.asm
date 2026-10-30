; pc486 - video test program (DOS .COM)
; Shows VBE information, then cycles through graphics modes.
; Press a key to go to the next mode.
        org 0x100
        bits 16

start:
        mov ax, 0x0003
        int 0x10
        ; --- VBE controller info ---
        mov di, vbeinfo
        mov dword [di], 'VBE2'
        mov ax, 0x4f00
        int 0x10
        cmp ax, 0x004f
        je .vbe_ok
        mov si, msg_novbe
        call puts
        jmp modes
.vbe_ok:
        mov si, msg_vbe
        call puts
        mov ax, [vbeinfo + 4]
        call puthex16
        mov si, msg_mem
        call puts
        mov ax, [vbeinfo + 18]       ; total memory in 64K blocks
        call puthex16
        mov si, msg_oem
        call puts
        push ds
        lds si, [vbeinfo + 6]
        call puts
        pop ds
        call crlf
        ; list modes
        mov si, msg_modes
        call puts
        push ds
        lds si, [vbeinfo + 14]
        mov cx, 60
.ml:    lodsw
        cmp ax, 0xffff
        je .mdone
        push ds
        push si
        push cs
        pop ds
        call puthex16
        mov al, ' '
        call putc
        pop si
        pop ds
        loop .ml
.mdone: pop ds
        call crlf
        ; info about mode 0x101
        mov cx, 0x101
        call modeinfo
        mov cx, 0x111
        call modeinfo
        mov cx, 0x112
        call modeinfo
        call waitkey

modes:
        ; --- mode 13h ---
        mov ax, 0x0013
        int 0x10
        push 0xa000
        pop es
        xor di, di
        xor dx, dx              ; y
.y13:   xor cx, cx              ; x
.x13:   mov ax, cx
        add ax, dx
        stosb
        inc cx
        cmp cx, 320
        jb .x13
        inc dx
        cmp dx, 200
        jb .y13
        call waitkey

        ; --- mode 12h: 16 vertical bars via BIOS pixel writes ---
        mov ax, 0x0012
        int 0x10
        mov bx, 640
        mov bp, 480
        call bars16
        call waitkey

        ; --- mode 0Dh ---
        mov ax, 0x000d
        int 0x10
        mov bx, 320
        mov bp, 200
        call bars16
        call waitkey

        ; --- mode 04h (CGA 4 colours) ---
        mov ax, 0x0004
        int 0x10
        mov bx, 320
        mov bp, 200
        call bars16
        call waitkey

        ; --- VESA 101h 640x480x256, banked ---
        mov ax, 0x4f02
        mov bx, 0x101
        int 0x10
        cmp ax, 0x004f
        jne .skip101
        mov dx, 0               ; bank
.b101:  push dx
        mov ax, 0x4f05
        xor bx, bx
        int 0x10
        pop dx
        push 0xa000
        pop es
        xor di, di
        mov cx, 0x8000
.f101:  mov ax, di
        shr ax, 8
        add al, dl
        shl dl, 4
        add al, dl
        shr dl, 4
        mov ah, al
        stosw
        loop .f101
        inc dx
        cmp dx, 5
        jb .b101
        call waitkey
.skip101:

        ; --- VESA 111h 640x480x64K ---
        mov ax, 0x4f02
        mov bx, 0x111
        int 0x10
        cmp ax, 0x004f
        jne .skip111
        mov dx, 0
.b111:  push dx
        mov ax, 0x4f05
        xor bx, bx
        int 0x10
        pop dx
        push 0xa000
        pop es
        xor di, di
        mov cx, 0x8000
.f111:  ; pixel index = (bank * 65536 + di) / 2 ; colour from x/y-ish bits
        mov ax, di
        shr ax, 1
        mov bx, dx
        shl bx, 15
        add ax, bx              ; low 16 bits of pixel index
        stosw
        loop .f111
        inc dx
        cmp dx, 10
        jb .b111
        call waitkey
.skip111:

        ; --- VESA 112h 640x480x16M ---
        mov ax, 0x4f02
        mov bx, 0x112
        int 0x10
        cmp ax, 0x004f
        jne .skip112
        mov dx, 0
.b112:  push dx
        mov ax, 0x4f05
        xor bx, bx
        int 0x10
        pop dx
        push 0xa000
        pop es
        xor di, di
        mov cx, 0x8000
.f112:  mov ax, di
        xor al, dl
        mov ah, dl
        shl ah, 4
        stosw
        loop .f112
        inc dx
        cmp dx, 19
        jb .b112
        call waitkey
.skip112:

        ; --- VESA 109h 132x25 text ---
        mov ax, 0x4f02
        mov bx, 0x109
        int 0x10
        cmp ax, 0x004f
        jne .skip109
        mov si, msg_132
        call puts
        call waitkey
.skip109:
        mov ax, 0x0003
        int 0x10
        mov si, msg_done
        call puts
        ret

; ---- helpers ----
; bars16: draws 16 vertical bars, width bx pixels total, height bp
bars16:
        xor dx, dx              ; row
.row:   xor cx, cx              ; col
.col:   mov ax, cx
        push dx
        xor dx, dx
        push bx
        shr bx, 4               ; bar width
        div bx
        pop bx
        pop dx
        mov ah, 0x0c
        push bx
        xor bh, bh
        int 0x10
        pop bx
        inc cx
        cmp cx, bx
        jb .col
        add dx, 1
        cmp dx, bp
        jb .row
        ret

; modeinfo: cx = mode, prints "mode wwwxhhh bpp attr lfb"
modeinfo:
        push cx
        mov ax, 0x4f01
        mov di, mibuf
        int 0x10
        pop cx
        cmp ax, 0x004f
        jne .bad
        mov ax, cx
        call puthex16
        mov al, ':'
        call putc
        mov ax, [mibuf + 18]
        call putdec
        mov al, 'x'
        call putc
        mov ax, [mibuf + 20]
        call putdec
        mov al, 'x'
        call putc
        xor ah, ah
        mov al, [mibuf + 25]
        call putdec
        mov si, msg_attr
        call puts
        mov ax, [mibuf]
        call puthex16
        mov si, msg_lfb
        call puts
        mov ax, [mibuf + 42]
        call puthex16
        mov ax, [mibuf + 40]
        call puthex16
        mov si, msg_bpl
        call puts
        mov ax, [mibuf + 16]
        call putdec
        call crlf
        ret
.bad:   mov si, msg_bad
        call puts
        ret

waitkey:
        xor ah, ah
        int 0x16
        ret

puts:   lodsb
        or al, al
        jz .e
        call putc
        jmp puts
.e:     ret

putc:   push bx
        mov ah, 0x0e
        mov bx, 7
        int 0x10
        pop bx
        ret

crlf:   mov al, 13
        call putc
        mov al, 10
        jmp putc

puthex16:
        push cx
        mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     call putc
        pop ax
        loop .h
        pop cx
        ret

putdec: push cx
        push dx
        xor cx, cx
        mov bx, 10
.p1:    xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .p1
.p2:    pop ax
        add al, '0'
        call putc
        loop .p2
        pop dx
        pop cx
        ret

msg_novbe: db "No VBE", 13, 10, 0
msg_vbe:   db "VBE version ", 0
msg_mem:   db "  memory(64K) ", 0
msg_oem:   db "  OEM: ", 0
msg_modes: db "Modes: ", 0
msg_attr:  db " attr ", 0
msg_lfb:   db " lfb ", 0
msg_bpl:   db " bpl ", 0
msg_bad:   db "modeinfo failed", 13, 10, 0
msg_132:   db "132-column text mode test line ....................................................................... end", 0
msg_done:  db "vtest done", 13, 10, 0

vbeinfo:   times 512 db 0
mibuf:     times 256 db 0
