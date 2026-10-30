/*
 * pc486 - 16450 UART with optional Microsoft serial mouse attached
 */
#include <stdlib.h>
#include <string.h>
#include "uart.h"

uint32_t get_uticks(void);

struct UART {
    uint8_t rbr, ier, lcr, mcr, lsr, msr, scr, dll, dlm;
    int thr_int;
    uint8_t q[256];
    int qh, qt;
    int irq, level;
    void *pic;
    void (*set_irq)(void *pic, int irq, int level);
    int mouse;
    int mdx, mdy, mbtn, mbtn_sent;
    uint32_t next_rx;
};

static int qlen(UART *u) { return (u->qt - u->qh) & 255; }
static void qput(UART *u, uint8_t b) { if (qlen(u) < 255) { u->q[u->qt] = b; u->qt = (u->qt + 1) & 255; } }

static void update_irq(UART *u)
{
    int lvl = 0;
    if ((u->ier & 1) && (u->lsr & 1)) lvl = 1;
    if ((u->ier & 2) && u->thr_int) lvl = 1;
    /* OUT2 gates the interrupt line on PCs */
    if (!(u->mcr & 0x08)) lvl = 0;
    if (lvl != u->level) {
        u->level = lvl;
        u->set_irq(u->pic, u->irq, lvl);
    }
}

static void fill_rbr(UART *u)
{
    if (!(u->lsr & 1) && qlen(u) && (int32_t)(get_uticks() - u->next_rx) >= 0) {
        u->rbr = u->q[u->qh];
        u->qh = (u->qh + 1) & 255;
        u->lsr |= 1;
        /* 1200 baud, 7N1 ~ 8.3 ms per byte; be a bit faster */
        u->next_rx = get_uticks() + 1000;
        update_irq(u);
    }
}

uint8_t uart_read(UART *u, int reg)
{
    uint8_t v = 0xff;
    switch (reg & 7) {
    case 0:
        if (u->lcr & 0x80) return u->dll;
        v = u->rbr;
        u->lsr &= ~1;
        update_irq(u);
        fill_rbr(u);
        return v;
    case 1:
        if (u->lcr & 0x80) return u->dlm;
        return u->ier;
    case 2:
        if ((u->ier & 1) && (u->lsr & 1)) v = 0x04;
        else if ((u->ier & 2) && u->thr_int) { v = 0x02; u->thr_int = 0; update_irq(u); }
        else v = 0x01;
        return v;
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5: return u->lsr | 0x60;
    case 6: {
        uint8_t m = 0;
        if (u->mcr & 0x10) {
            /* loopback */
            m = ((u->mcr & 2) << 3) | ((u->mcr & 1) << 5) | ((u->mcr & 4) << 4) | ((u->mcr & 8) << 4);
        } else if (u->mouse) {
            m = 0x30 | 0x80; /* CTS, DSR, DCD */
        }
        return m;
    }
    case 7: return u->scr;
    }
    return v;
}

void uart_write(UART *u, int reg, uint8_t val)
{
    switch (reg & 7) {
    case 0:
        if (u->lcr & 0x80) { u->dll = val; return; }
        u->thr_int = 1;
        update_irq(u);
        return;
    case 1:
        if (u->lcr & 0x80) { u->dlm = val; return; }
        u->ier = val & 0x0f;
        if (u->ier & 2) u->thr_int = 1;
        update_irq(u);
        return;
    case 3: u->lcr = val; return;
    case 4: {
        uint8_t old = u->mcr;
        u->mcr = val & 0x1f;
        /* mouse reset on RTS rising edge: identify as Microsoft mouse */
        if (u->mouse && (val & 2) && !(old & 2)) {
            u->qh = u->qt = 0;
            u->lsr &= ~1;
            qput(u, 'M');
            u->next_rx = get_uticks() + 5000;
        }
        update_irq(u);
        return;
    }
    case 7: u->scr = val; return;
    }
}

void uart_mouse_event(UART *u, int dx, int dy, int buttons)
{
    u->mdx += dx;
    u->mdy += dy;
    u->mbtn = buttons;
}

void uart_step(UART *u)
{
    if (u->mouse && qlen(u) == 0 && !(u->lsr & 1) && (u->mcr & 2) &&
        (u->mdx || u->mdy || u->mbtn != u->mbtn_sent)) {
        int dx = u->mdx, dy = u->mdy;
        if (dx > 127) dx = 127;
        if (dx < -128) dx = -128;
        if (dy > 127) dy = 127;
        if (dy < -128) dy = -128;
        u->mdx -= dx;
        u->mdy -= dy;
        int lb = u->mbtn & 1, rb = (u->mbtn >> 1) & 1;
        qput(u, 0x40 | (lb << 5) | (rb << 4) | (((dy >> 6) & 3) << 2) | ((dx >> 6) & 3));
        qput(u, dx & 0x3f);
        qput(u, dy & 0x3f);
        u->mbtn_sent = u->mbtn;
    }
    fill_rbr(u);
}

UART *uart_new(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level), int mouse)
{
    UART *u = calloc(1, sizeof(UART));
    u->irq = irq;
    u->pic = pic;
    u->set_irq = set_irq;
    u->mouse = mouse;
    u->lsr = 0x60;
    return u;
}
