/*
 * pc486 - minimal PCI bus (configuration mechanism #1 at CF8h/CFCh)
 *
 *   00:00.0  ALi M1435 PCI-VLB bridge (the companion of the M1429)
 *   00:0A.0  S3 ViRGE 86C325 (Diamond Stealth 3D 2000)
 */
#include <string.h>
#include "pcibus.h"

#define VIRGE_DEVFN (0x0a << 3)

static void put16(uint8_t *c, int o, uint16_t v) { c[o] = v; c[o + 1] = v >> 8; }
static void put32(uint8_t *c, int o, uint32_t v) { put16(c, o, v); put16(c, o + 2, v >> 16); }
static uint32_t get32(const uint8_t *c, int o) { return c[o] | (c[o+1] << 8) | (c[o+2] << 16) | ((uint32_t)c[o+3] << 24); }

void pcibus_init(PCIBusState *p, void *vga_opaque,
                 void (*set_lfb)(void *opaque, uint32_t base, int enabled))
{
    memset(p, 0, sizeof(*p));
    p->vga_opaque = vga_opaque;
    p->set_lfb = set_lfb;

    uint8_t *h = p->host;
    put16(h, 0x00, 0x10b9);     /* ALi */
    put16(h, 0x02, 0x1435);     /* M1435 */
    put16(h, 0x04, 0x0007);
    put16(h, 0x06, 0x0200);
    h[0x08] = 0x00;
    h[0x0b] = 0x06;             /* bridge */
    h[0x0a] = 0x00;             /* host bridge */

    uint8_t *v = p->virge;
    put16(v, 0x00, 0x5333);     /* S3 */
    put16(v, 0x02, 0x5631);     /* ViRGE */
    put16(v, 0x04, 0x0003);     /* I/O + memory enabled (no PCI BIOS to do it) */
    put16(v, 0x06, 0x0200);
    v[0x08] = 0x06;             /* revision */
    v[0x09] = 0x00;
    v[0x0a] = 0x00;             /* VGA compatible controller */
    v[0x0b] = 0x03;
    v[0x0e] = 0x00;
    put32(v, 0x10, 0xe0000000); /* BAR0: 64 MB, frame buffer + MMIO */
    put16(v, 0x2c, 0x1092);     /* subsystem: Diamond Multimedia */
    put16(v, 0x2e, 0x2200);
    v[0x3c] = 0x0b;             /* interrupt line */
    v[0x3d] = 0x01;             /* INTA# */
}

static uint8_t *dev_of(PCIBusState *p, uint32_t addr)
{
    if (!(addr & 0x80000000u))
        return NULL;
    if (((addr >> 16) & 0xff) != 0)
        return NULL;
    int devfn = (addr >> 8) & 0xff;
    if (devfn == 0)
        return p->host;
    if (devfn == VIRGE_DEVFN)
        return p->virge;
    return NULL;
}

uint32_t pcibus_read(PCIBusState *p, int port, int size)
{
    if (port >= 0xcf8 && port <= 0xcfb) {
        uint32_t v = p->addr >> ((port & 3) * 8);
        return size == 4 ? p->addr : size == 2 ? (v & 0xffff) : (v & 0xff);
    }
    uint8_t *c = dev_of(p, p->addr);
    int reg = (p->addr & 0xfc) + (port & 3);
    if (!c)
        return size == 4 ? 0xffffffffu : size == 2 ? 0xffff : 0xff;
    uint32_t v = 0;
    for (int i = 0; i < size && reg + i < 256; i++)
        v |= (uint32_t)c[reg + i] << (i * 8);
    return v;
}

static void virge_write(PCIBusState *p, int reg, uint8_t val)
{
    uint8_t *v = p->virge;
    switch (reg) {
    case 0x04:
        v[reg] = val & 0x27;
        break;
    case 0x05: case 0x06:
        break;
    case 0x07:
        v[reg] &= ~(val & 0xf9); /* write 1 to clear */
        break;
    case 0x10: case 0x11: case 0x12:
        v[reg] = 0;              /* 64 MB alignment */
        break;
    case 0x13:
        v[reg] = val & 0xfc;
        break;
    case 0x3c:
        v[reg] = val;
        break;
    case 0x30: case 0x31: case 0x32: case 0x33:
        v[reg] = 0;              /* no expansion ROM BAR */
        break;
    default:
        if (reg >= 0x40)
            v[reg] = val;
        break;
    }
}

void pcibus_write(PCIBusState *p, int port, uint32_t val, int size)
{
    if (port >= 0xcf8 && port <= 0xcfb) {
        if (size == 4)
            p->addr = val & 0x80fffffc;
        return;
    }
    uint8_t *c = dev_of(p, p->addr);
    int reg = (p->addr & 0xfc) + (port & 3);
    if (!c)
        return;
    for (int i = 0; i < size && reg + i < 256; i++) {
        uint8_t b = val >> (i * 8);
        if (c == p->virge)
            virge_write(p, reg + i, b);
        else if (reg + i >= 0x40)
            c[reg + i] = b;
    }
    if (c == p->virge && reg <= 0x13 && reg + size > 0x04 && p->set_lfb)
        p->set_lfb(p->vga_opaque, get32(p->virge, 0x10) & 0xfc000000u,
                   (p->virge[0x04] & 2) != 0);
}
