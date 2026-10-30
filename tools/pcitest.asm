; pc486 - PCI BIOS test
        org 0x100
        bits 16
        cpu 386
        mov ax, 0xb101
        xor edi, edi
        int 0x1a
        jc .nopci
        mov si, m1
        call puts
        mov eax, edx
        call hex32
        mov si, m2
        call puts
        mov ax, bx
        call hex16
        call crlf
        ; find S3 ViRGE
        mov ax, 0xb102
        mov cx, 0x5631
        mov dx, 0x5333
        xor si, si
        int 0x1a
        jc .nodev
        mov si, m3
        call puts
        mov ax, bx
        call hex16
        push bx
        mov si, m4
        call puts
        pop bx
        push bx
        mov ax, 0xb10a
        mov di, 0x10
        int 0x1a
        mov eax, ecx
        call hex32
        call crlf
        pop bx
        ; find VGA class
        mov ax, 0xb103
        mov ecx, 0x030000
        xor si, si
        int 0x1a
        jc .nodev
        mov si, m5
        call puts
        mov ax, bx
        call hex16
        call crlf
        ret
.nopci: mov si, m6
        call puts
        ret
.nodev: mov si, m7
        call puts
        ret
puts:   lodsb
        or al, al
        jz .e
        mov ah, 0x0e
        int 0x10
        jmp puts
.e:     ret
crlf:   mov si, mcr
        jmp puts
hex32:  push eax
        shr eax, 16
        call hex16
        pop eax
hex16:  mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     mov ah, 0x0e
        int 0x10
        pop ax
        loop .h
        ret
m1: db "PCI BIOS: sig ",0
m2: db " ver ",0
m3: db "ViRGE at bus/devfn ",0
m4: db " BAR0 ",0
m5: db "VGA class device at ",0
m6: db "no PCI BIOS",13,10,0
m7: db "device not found",13,10,0
mcr: db 13,10,0
