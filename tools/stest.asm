; pc486 sound test: AdLib note, SB DSP reset, SB IRQ (F2), SB 8-bit DMA playback
org 100h
start:
    mov dx, msg_opl
    call puts
    ; OPL2 channel 0: simple tone
    mov si, opl_tab
.ol: lodsw
    cmp ax, 0FFFFh
    je .od
    call oplw
    jmp .ol
.od:
    mov cx, 18          ; ~1 s
    call wait_ticks
    mov ax, 0B000h | 011h   ; key off (reg B0 = 11)
    call oplw

    ; --- SB DSP reset
    mov dx, 226h
    mov al, 1
    out dx, al
    mov cx, 100
.d1: in al, dx
    loop .d1
    xor al, al
    out dx, al
    mov cx, 1000
.d2: mov dx, 22Eh
    in al, dx
    test al, 80h
    jnz .d3
    loop .d2
    mov dx, msg_noreset
    call puts
    jmp exit
.d3: mov dx, 22Ah
    in al, dx
    cmp al, 0AAh
    je .d4
    mov dx, msg_noreset
    call puts
    jmp exit
.d4: mov dx, msg_reset_ok
    call puts

    ; hook IRQ5 = INT 0Dh
    cli
    xor ax, ax
    mov es, ax
    mov ax, [es:0Dh*4]
    mov [old_off], ax
    mov ax, [es:0Dh*4+2]
    mov [old_seg], ax
    mov word [es:0Dh*4], isr
    mov [es:0Dh*4+2], cs
    in al, 21h
    and al, 0DFh
    out 21h, al
    sti

    ; IRQ test via F2
    mov byte [irqflag], 0
    mov al, 0F2h
    call dspw
    mov cx, 5
    call wait_irq
    jc .f2bad
    mov dx, msg_f2_ok
    call puts
    jmp .dma
.f2bad: mov dx, msg_f2_bad
    call puts

.dma:
    ; fill buffer with square wave
    push ds
    pop es
    mov di, buf
    mov cx, BUFLEN
    xor bx, bx
.fl: mov al, 40h
    test bx, 16
    jz .f2
    mov al, 0C0h
.f2: stosb
    inc bx
    loop .fl
    ; speaker on
    mov al, 0D1h
    call dspw
    ; time constant 11025 Hz -> 165
    mov al, 40h
    call dspw
    mov al, 165
    call dspw
    ; DMA channel 1 setup
    mov ax, ds
    mov dx, ax
    shl ax, 4
    shr dx, 12
    add ax, buf
    adc dx, 0
    mov bx, ax          ; offset
    mov al, 5           ; mask ch1
    out 0Ah, al
    xor al, al
    out 0Ch, al         ; clear flip-flop
    mov al, 49h         ; single, read (mem->io), ch1
    out 0Bh, al
    mov al, bl
    out 02h, al
    mov al, bh
    out 02h, al
    mov al, dl
    out 83h, al         ; page ch1
    mov ax, BUFLEN-1
    out 03h, al
    mov al, ah
    out 03h, al
    mov al, 1
    out 0Ah, al         ; unmask ch1
    mov byte [irqflag], 0
    mov al, 14h
    call dspw
    mov ax, BUFLEN-1
    call dspw
    mov al, ah
    call dspw
    mov cx, 60          ; wait up to ~3 s
    call wait_irq
    jc .dbad
    mov dx, msg_dma_ok
    call puts
    jmp .restore
.dbad: mov dx, msg_dma_bad
    call puts
.restore:
    cli
    xor ax, ax
    mov es, ax
    mov ax, [old_off]
    mov [es:0Dh*4], ax
    mov ax, [old_seg]
    mov [es:0Dh*4+2], ax
    in al, 21h
    or al, 20h
    out 21h, al
    sti
exit:
    mov ax, 4C00h
    int 21h

isr: push ax
    push dx
    mov dx, 22Eh
    in al, dx
    mov byte [cs:irqflag], 1
    mov al, 20h
    out 20h, al
    pop dx
    pop ax
    iret

; wait for irqflag, cx = ticks timeout; CF=1 on timeout
wait_irq:
    push es
    xor ax, ax
    mov es, ax
    mov bx, [es:46Ch]
.w: cmp byte [irqflag], 0
    jne .ok
    mov ax, [es:46Ch]
    sub ax, bx
    cmp ax, cx
    jb .w
    pop es
    stc
    ret
.ok: pop es
    clc
    ret

wait_ticks:
    push es
    xor ax, ax
    mov es, ax
    mov bx, [es:46Ch]
.w: mov ax, [es:46Ch]
    sub ax, bx
    cmp ax, cx
    jb .w
    pop es
    ret

dspw: push dx
    push ax
    mov dx, 22Ch
.w: in al, dx
    test al, 80h
    jnz .w
    pop ax
    out dx, al
    pop dx
    ret

; ah = reg, al = value
oplw: push dx
    push cx
    push ax
    mov dx, 388h
    mov al, ah
    out dx, al
    mov cx, 6
.a: in al, dx
    loop .a
    pop ax
    inc dx
    out dx, al
    dec dx
    mov cx, 35
.b: in al, dx
    loop .b
    pop cx
    pop dx
    ret

puts: mov ah, 9
    int 21h
    ret

opl_tab:
    dw 2001h, 2321h, 401Fh, 4300h, 60F0h, 63F0h, 8077h, 8377h, 0A098h, 0C001h, 0B031h
    dw 0FFFFh
irqflag db 0
old_off dw 0
old_seg dw 0
msg_opl db 'AdLib: nota per 1 s', 13, 10, '$'
msg_noreset db 'SB: reset DSP FALLITO', 13, 10, '$'
msg_reset_ok db 'SB: reset DSP ok', 13, 10, '$'
msg_f2_ok db 'SB: IRQ 5 (F2) ok', 13, 10, '$'
msg_f2_bad db 'SB: IRQ 5 (F2) NON ARRIVA', 13, 10, '$'
msg_dma_ok db 'SB: DMA 8 bit ok, IRQ di fine blocco ricevuto', 13, 10, '$'
msg_dma_bad db 'SB: DMA 8 bit: IRQ NON ARRIVA', 13, 10, '$'
BUFLEN equ 11025
buf:
