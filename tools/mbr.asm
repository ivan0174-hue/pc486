; pc486 - minimal standard MBR: boots the active primary partition
        org 0x600
        bits 16
start:  cli
        xor ax, ax
        mov ss, ax
        mov sp, 0x7c00
        mov ds, ax
        mov es, ax
        mov si, 0x7c00
        mov di, 0x600
        mov cx, 256
        cld
        rep movsw
        jmp 0:relocated
relocated:
        sti
        mov [drive], dl
        mov si, 0x600 + 446
        mov cx, 4
find:   cmp byte [si], 0x80
        je found
        add si, 16
        loop find
        mov si, msg_noact
        jmp error
found:  mov di, 5               ; retries
read:   mov dl, [drive]
        mov dh, [si + 1]        ; head
        mov cx, [si + 2]        ; cylinder/sector
        mov bx, 0x7c00
        mov ax, 0x0201
        push si
        int 0x13
        pop si
        jnc ok
        xor ax, ax
        int 0x13
        dec di
        jnz read
        mov si, msg_err
        jmp error
ok:     cmp word [0x7dfe], 0xaa55
        jne nosys
        mov dl, [drive]
        jmp 0:0x7c00
nosys:  mov si, msg_nosys
error:  lodsb
        or al, al
        jz $
        mov ah, 0x0e
        mov bx, 7
        int 0x10
        jmp error
drive:  db 0x80
msg_noact: db "No active partition", 0
msg_err:   db "Error loading OS", 0
msg_nosys: db "Missing OS", 0
        times 446 - ($ - $$) db 0
