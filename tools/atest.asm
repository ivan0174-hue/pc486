; pc486 - S3 ViRGE 2D engine test (DOS .COM), 640x480x256
        org 0x100
        bits 16
        cpu 386

%macro WREG 2           ; register offset, value (32 bit)
        mov eax, %2
        mov [es:%1], eax
%endmacro

CMD_XP   equ 1 << 25
CMD_YP   equ 1 << 26
BLT      equ 0 << 27
FILL     equ 2 << 27
LINE     equ 3 << 27
%define ROP(r) ((r) << 17)

start:
        mov ax, 0x4f02
        mov bx, 0x101
        int 0x10
        ; unlock S3 registers and enable MMIO at A0000
        mov dx, 0x3d4
        mov ax, 0x4838
        out dx, ax
        mov ax, 0xa539
        out dx, ax
        mov al, 0x53
        out dx, al
        inc dx
        in al, dx
        or al, 0x08
        out dx, al
        push 0xa000
        pop es

        ; common: bases, stride 640, no clipping
        WREG 0xa4d4, 0
        WREG 0xa4d8, 0
        WREG 0xa4e4, (640 << 16) | 640

        ; 1. solid rectangle fill, colour 4, at (20,20) 200x100
        WREG 0xa4f4, 4
        WREG 0xa504, (199 << 16) | 100
        WREG 0xa50c, (20 << 16) | 20
        WREG 0xa500, FILL | CMD_XP | CMD_YP | ROP(0xf0)

        ; a white frame inside it via a second fill
        WREG 0xa4f4, 15
        WREG 0xa504, (99 << 16) | 50
        WREG 0xa50c, (70 << 16) | 45
        WREG 0xa500, FILL | CMD_XP | CMD_YP | ROP(0xf0)

        ; 2. screen to screen copy of that area to (300,20)
        WREG 0xa504, (199 << 16) | 100
        WREG 0xa508, (20 << 16) | 20
        WREG 0xa50c, (300 << 16) | 20
        WREG 0xa500, BLT | CMD_XP | CMD_YP | ROP(0xcc)

        ; 3. mono pattern fill (checkerboard), fg 2 bg 14, at (20,200)
        WREG 0xa4e8, 0x55aa55aa
        WREG 0xa4ec, 0x55aa55aa
        WREG 0xa4f4, 2
        WREG 0xa4f0, 14
        WREG 0xa504, (199 << 16) | 100
        WREG 0xa50c, (20 << 16) | 200
        WREG 0xa500, FILL | CMD_XP | CMD_YP | ROP(0xf0) | (1 << 8)

        ; 4. monochrome host data expansion (text-like), 32x16 at (300,200)
        ;    fg 15, bg 1, dword aligned lines
        WREG 0xa4fc, 15
        WREG 0xa4f8, 1
        WREG 0xa504, (31 << 16) | 16
        WREG 0xa50c, (300 << 16) | 200
        WREG 0xa500, BLT | CMD_XP | CMD_YP | ROP(0xcc) | (1 << 7) | (1 << 6) | (2 << 10)
        mov si, glyph
        mov cx, 16
.hd:    lodsd
        mov [es:0], eax
        loop .hd

        ; 5. line (diagonal) from (400,450) going up and right, colour 12
        WREG 0xa8f4, 12
        WREG 0xa96c, (400 << 16) | 499
        WREG 0xa970, 1 << 20
        WREG 0xa974, 400 << 20
        WREG 0xa978, 450
        WREG 0xa900, LINE | ROP(0xf0)
        WREG 0xa97c, 100 | (1 << 31)
        ; (command is executed when CMD is written without AE, but line
        ;  parameters must be in place first: write the command again)
        WREG 0xa900, LINE | ROP(0xf0)

        ; 6. XOR rectangle (pattern ^ dest) over part of the first copy
        WREG 0xa4f4, 0x0f
        WREG 0xa504, (99 << 16) | 60
        WREG 0xa50c, (350 << 16) | 60
        WREG 0xa500, FILL | CMD_XP | CMD_YP | ROP(0x5a)

        ; 7. clipped fill: clip rectangle (100..199, 350..399)
        WREG 0xa4dc, (100 << 16) | 199
        WREG 0xa4e0, (350 << 16) | 399
        WREG 0xa4f4, 9
        WREG 0xa504, (299 << 16) | 120
        WREG 0xa50c, (20 << 16) | 320
        WREG 0xa500, FILL | CMD_XP | CMD_YP | ROP(0xf0) | 2

        ; wait for the engine
.idle:  mov eax, [es:0x8504]
        test eax, 1 << 13
        jz .idle

        xor ah, ah
        int 0x16
        mov ax, 3
        int 0x10
        ret

glyph:  ; 32 pixels per row, bytes in memory order MSB = leftmost pixel
        db 0xff, 0x00, 0xff, 0x00
        db 0x80, 0x00, 0x00, 0x01
        db 0x80, 0x00, 0x00, 0x01
        db 0x80, 0x18, 0x18, 0x01
        db 0x80, 0x3c, 0x3c, 0x01
        db 0x80, 0x7e, 0x7e, 0x01
        db 0x80, 0xff, 0xff, 0x01
        db 0x80, 0xff, 0xff, 0x01
        db 0x80, 0x7e, 0x7e, 0x01
        db 0x80, 0x3c, 0x3c, 0x01
        db 0x80, 0x18, 0x18, 0x01
        db 0x80, 0x00, 0x00, 0x01
        db 0x80, 0x00, 0x00, 0x01
        db 0x80, 0x00, 0x00, 0x01
        db 0x80, 0x00, 0x00, 0x01
        db 0xff, 0xff, 0xff, 0xff
