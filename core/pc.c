/*
 * pc486 - board: AMI 486 BIOS on ALi M1429 chipset, ISA bus,
 *         S3 ViRGE video, IDE, 82077 floppy, MC146818 RTC, 8042 KBC,
 *         optional SoundBlaster 16 + OPL, serial mouse on COM1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "pc.h"
#include "pcibios_rom.h"

#define PC_STEP_COUNT 4096

/* ------------------------------------------------------------------ */
/* interrupt glue                                                     */

static void raise_irq_cb(void *o, PicState2 *s)
{
    (void)s;
    cpui386_raise_irq(o);
}

static int read_irq_cb(void *o)
{
    return i8259_read_irq(o);
}

#ifdef PC486_DEBUG_DE
PC *g_dbg_pc;
uint8_t *pc486_dbg_ram(void) { return g_dbg_pc->ram; }
#endif
static void set_irq_cb(void *o, int irq, int level)
{
#ifdef PC486_DEBUG_DE
    if (irq == 0 && level && getenv("PITDBG")) {
        static uint64_t lc; static uint32_t lt;
        uint64_t c = cpui386_get_cycle(g_dbg_pc->cpu); uint32_t t = get_uticks();
        fprintf(stderr, "irq0 dt=%u us dI=%llu\n", t - lt, (unsigned long long)(c - lc));
        lc = c; lt = t;
    }
#endif
    i8259_set_irq(o, irq, level);
}

/* ------------------------------------------------------------------ */
/* ALi M1429 chipset: shadow RAM control                              */

static void shadow_recalc(PC *pc)
{
    for (int i = 0; i < 8; i++) {
        int rd = 0, wr = 0;
        if (pc->ali[0x13] & (1 << i)) {
            rd = (pc->ali[0x14] & 0x01) != 0;
            wr = (pc->ali[0x14] & 0x02) != 0;
        }
        uint32_t off = i << 15;
        if (rd != pc->blk_read_ram[i]) {
            memcpy(pc->ram + 0xc0000 + off, rd ? pc->shadow + off : pc->rom + off,
                   0x8000);
            pc->blk_read_ram[i] = rd;
        }
        pc->blk_write_ram[i] = wr;
    }
}

static void ali_write(PC *pc, int port, uint8_t val)
{
    if (port == 0x22) {
        pc->ali_idx = val;
        return;
    }
    /* registers are locked unless index 3 holds the key 0xC5 */
    if (pc->ali_idx != 0x03 && pc->ali[0x03] != 0xc5)
        return;
    pc->ali[pc->ali_idx] = val;
    if (pc->ali_idx == 0x13 || pc->ali_idx == 0x14)
        shadow_recalc(pc);
}

static uint8_t ali_read(PC *pc, int port)
{
    if (port == 0x23) {
        if (pc->ali_idx == 0x03 || (pc->ali_idx >= 0x10 && pc->ali_idx <= 0x4a))
            return pc->ali[pc->ali_idx];
    }
    return 0xff;
}

/* ------------------------------------------------------------------ */
/* I/O space                                                          */

static uint8_t io_read8_(PC *pc, int port);
static uint8_t io_read8(void *o, int port)
{
    PC *pc = o;
    uint8_t v = io_read8_(pc, port);
    if (port >= pc->trace_lo && port <= pc->trace_hi)
        plat_log("in  %03x -> %02x\n", port, v);
    return v;
}

static uint8_t io_read8_(PC *pc, int port)
{
    if (pc->has_pci && port >= 0xcf8 && port <= 0xcff)
        return pcibus_read(&pc->pcibus, port, 1);
    switch (port) {
    case 0x00 ... 0x07:
        return i8257_read_chan(pc->dma, port, 1);
    case 0x08 ... 0x0f:
        return i8257_read_cont(pc->dma, port - 0x08, 1);
    case 0x20: case 0x21: case 0xa0: case 0xa1:
        return i8259_ioport_read(pc->pic, port);
    case 0x22: case 0x23:
        return ali_read(pc, port);
    case 0x40 ... 0x43:
        return i8254_ioport_read(pc->pit, port);
    case 0x60:
        return kbd_read_data(pc->kbc, port);
    case 0x61:
        return pcspk_ioport_read(pc->spk);
    case 0x64:
        return kbd_read_status(pc->kbc, port);
    case 0x70 ... 0x73:
        return rtc_ioport_read(pc->rtc, port);
    case 0x81: case 0x82: case 0x83: case 0x87:
        return i8257_read_page(pc->dma, port - 0x80);
    case 0x89: case 0x8a: case 0x8b: case 0x8f:
        return i8257_read_page(pc->hdma, port - 0x88);
    case 0x80: case 0x84 ... 0x86: case 0x88: case 0x8c ... 0x8e:
        return pc->port80[port & 15];
    case 0x92:
        return pc->port92;
    case 0xc0 ... 0xdf:
        if (port & 1) return 0xff;
        if (port < 0xd0)
            return i8257_read_chan(pc->hdma, port - 0xc0, 1);
        return i8257_read_cont(pc->hdma, port - 0xd0, 1);
    case 0xf0 ... 0xff:
        return 0xff; /* coprocessor ports */
    case 0x170 ... 0x177:
        return pc->ide2 ? ide_ioport_read(pc->ide2, port - 0x170) : 0xff;
    case 0x1f0 ... 0x1f7:
        return ide_ioport_read(pc->ide, port - 0x1f0);
    case 0x376:
        return pc->ide2 ? ide_status_read(pc->ide2) : 0xff;
    case 0x3f6:
        return ide_status_read(pc->ide);
    case 0x3f0 ... 0x3f5: case 0x3f7:
        return fdc_ioport_read(pc->fdc, port);
    case 0x3b0 ... 0x3df:
        return s3v_io_read(pc->vga, port);
    case 0x3f8 ... 0x3ff:
        return uart_read(pc->com1, port - 0x3f8);
    case 0x2f8 ... 0x2ff:
        return uart_read(pc->com2, port - 0x2f8);
    case 0x220 ... 0x22f:
        if (!pc->sb16) return 0xff;
        switch (port & 15) {
        case 0x0: case 0x1: case 0x2: case 0x3: case 0x8: case 0x9:
            return adlib_read(pc->adlib, port);
        case 0x5:
            return sb16_mixer_read(pc->sb16, port);
        case 0x6: case 0xa: case 0xc: case 0xd: case 0xe: case 0xf:
            return sb16_dsp_read(pc->sb16, port);
        }
        return 0xff;
    case 0x388 ... 0x38b:
        return pc->adlib ? adlib_read(pc->adlib, port) : 0xff;
    }
    return 0xff;
}

static void io_write8(void *o, int port, uint8_t val)
{
    PC *pc = o;
    if (port >= pc->trace_lo && port <= pc->trace_hi)
        plat_log("out %03x <- %02x\n", port, val);
    if (pc->has_pci && port >= 0xcf8 && port <= 0xcff) {
        pcibus_write(&pc->pcibus, port, val, 1);
        return;
    }
    switch (port) {
    case 0x00 ... 0x07:
        i8257_write_chan(pc->dma, port, val, 1);
        return;
    case 0x08 ... 0x0f:
        i8257_write_cont(pc->dma, port - 0x08, val, 1);
        return;
    case 0x20: case 0x21: case 0xa0: case 0xa1:
        i8259_ioport_write(pc->pic, port, val);
        return;
    case 0x22: case 0x23:
        ali_write(pc, port, val);
        return;
    case 0x40 ... 0x43:
        i8254_ioport_write(pc->pit, port, val);
        return;
    case 0x60:
        kbd_write_data(pc->kbc, port, val);
        return;
    case 0x61:
        pcspk_ioport_write(pc->spk, val);
        return;
    case 0x64:
        kbd_write_command(pc->kbc, port, val);
        return;
    case 0x70 ... 0x73:
        rtc_ioport_write(pc->rtc, port, val);
        return;
    case 0x80:
        pc->port80[0] = val;
        pc->post_code = val;
        if (pc->post_log)
            plat_log("POST %02x\n", val);
        return;
    case 0x81: case 0x82: case 0x83: case 0x87:
        pc->port80[port & 15] = val;
        i8257_write_page(pc->dma, port - 0x80, val);
        return;
    case 0x89: case 0x8a: case 0x8b: case 0x8f:
        pc->port80[port & 15] = val;
        i8257_write_page(pc->hdma, port - 0x88, val);
        return;
    case 0x84 ... 0x86: case 0x88: case 0x8c ... 0x8e:
        pc->port80[port & 15] = val;
        return;
    case 0x92:
        pc->port92 = val & ~1;
        cpui386_set_a20(pc->cpu, (val >> 1) & 1);
        if (val & 1)
            pc->reset_request = 1;
        return;
    case 0xc0 ... 0xdf:
        if (port & 1) return;
        if (port < 0xd0)
            i8257_write_chan(pc->hdma, port - 0xc0, val, 1);
        else
            i8257_write_cont(pc->hdma, port - 0xd0, val, 1);
        return;
    case 0x170 ... 0x177:
        if (pc->ide2) ide_ioport_write(pc->ide2, port - 0x170, val);
        return;
    case 0x1f0 ... 0x1f7:
        ide_ioport_write(pc->ide, port - 0x1f0, val);
        return;
    case 0x376:
        if (pc->ide2) ide_cmd_write(pc->ide2, val);
        return;
    case 0x3f6:
        ide_cmd_write(pc->ide, val);
        return;
    case 0x3f0 ... 0x3f5: case 0x3f7:
        fdc_ioport_write(pc->fdc, port, val);
        return;
    case 0x3b0 ... 0x3df:
        s3v_io_write(pc->vga, port, val);
        return;
    case 0x3f8 ... 0x3ff:
        uart_write(pc->com1, port - 0x3f8, val);
        return;
    case 0x2f8 ... 0x2ff:
        uart_write(pc->com2, port - 0x2f8, val);
        return;
    case 0x220 ... 0x22f:
        if (!pc->sb16) return;
        switch (port & 15) {
        case 0x0: case 0x1: case 0x2: case 0x3: case 0x8: case 0x9:
            adlib_write(pc->adlib, port, val);
            return;
        case 0x4:
            sb16_mixer_write_indexb(pc->sb16, port, val);
            return;
        case 0x5:
            sb16_mixer_write_datab(pc->sb16, port, val);
            return;
        case 0x6: case 0xc:
            sb16_dsp_write(pc->sb16, port, val);
            return;
        }
        return;
    case 0x388 ... 0x38b:
        if (pc->adlib) adlib_write(pc->adlib, port, val);
        return;
    }
}

static uint16_t io_read16(void *o, int port)
{
    PC *pc = o;
    if (pc->has_pci && port >= 0xcf8 && port <= 0xcfe)
        return pcibus_read(&pc->pcibus, port, 2);
    switch (port) {
    case 0x1f0:
        return ide_data_readw(pc->ide);
    case 0x170:
        return pc->ide2 ? ide_data_readw(pc->ide2) : 0xffff;
    }
    return io_read8(o, port) | (io_read8(o, port + 1) << 8);
}

static void io_write16(void *o, int port, uint16_t val)
{
    PC *pc = o;
    if (pc->has_pci && port >= 0xcf8 && port <= 0xcfe) {
        pcibus_write(&pc->pcibus, port, val, 2);
        return;
    }
    switch (port) {
    case 0x1f0:
        ide_data_writew(pc->ide, val);
        return;
    case 0x170:
        if (pc->ide2) ide_data_writew(pc->ide2, val);
        return;
    }
    io_write8(o, port, val & 0xff);
    io_write8(o, port + 1, val >> 8);
}

static uint32_t io_read32(void *o, int port)
{
    PC *pc = o;
    if (pc->has_pci && (port == 0xcf8 || port == 0xcfc))
        return pcibus_read(&pc->pcibus, port, 4);
    switch (port) {
    case 0x1f0:
        return ide_data_readl(pc->ide);
    case 0x170:
        return pc->ide2 ? ide_data_readl(pc->ide2) : 0xffffffff;
    }
    return io_read16(o, port) | ((uint32_t)io_read16(o, port + 2) << 16);
}

static void io_write32(void *o, int port, uint32_t val)
{
    PC *pc = o;
    if (pc->has_pci && (port == 0xcf8 || port == 0xcfc)) {
        pcibus_write(&pc->pcibus, port, val, 4);
        return;
    }
    switch (port) {
    case 0x1f0:
        ide_data_writel(pc->ide, val);
        return;
    case 0x170:
        if (pc->ide2) ide_data_writel(pc->ide2, val);
        return;
    }
    io_write16(o, port, val & 0xffff);
    io_write16(o, port + 2, val >> 16);
}

static int io_read_string(void *o, int port, uint8_t *buf, int size, int count)
{
    PC *pc = o;
    switch (port) {
    case 0x1f0:
        return ide_data_read_string(pc->ide, buf, size, count);
    case 0x170:
        return pc->ide2 ? ide_data_read_string(pc->ide2, buf, size, count) : 0;
    }
    return 0;
}

static int io_write_string(void *o, int port, uint8_t *buf, int size, int count)
{
    PC *pc = o;
    switch (port) {
    case 0x1f0:
        return ide_data_write_string(pc->ide, buf, size, count);
    case 0x170:
        return pc->ide2 ? ide_data_write_string(pc->ide2, buf, size, count) : 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* memory-mapped I/O: A0000-BFFFF (video), C0000-FFFFF writes (ROM /   */
/* shadow), E0000000+ (linear frame buffer, BIOS alias at top)        */

static inline void rom_area_write8(PC *pc, uint32_t addr, uint8_t val)
{
    uint32_t off = addr - 0xc0000;
    int blk = off >> 15;
    if (pc->blk_write_ram[blk]) {
        pc->shadow[off] = val;
        if (pc->blk_read_ram[blk])
            pc->ram[addr] = val;
    }
}

static uint8_t iomem_read8(void *o, uword addr)
{
    PC *pc = o;
    if (addr < 0xc0000)
        return s3v_mem_read8(pc->vga, addr);
    if (addr >= 0xfffe0000u)
        return pc->rom[0x30000 + (addr & 0xffff)];
    if (s3v_lfb_hit(pc->vga, addr))
        return s3v_lfb_read8(pc->vga, addr);
    return 0xff;
}

static void iomem_write8(void *o, uword addr, uint8_t val)
{
    PC *pc = o;
    if (addr < 0xc0000) {
        s3v_mem_write8(pc->vga, addr, val);
        return;
    }
    if (addr < 0x100000) {
        rom_area_write8(pc, addr, val);
        return;
    }
    if (s3v_lfb_hit(pc->vga, addr))
        s3v_lfb_write8(pc->vga, addr, val);
}

static uint16_t iomem_read16(void *o, uword addr)
{
    PC *pc = o;
    if (addr < 0xc0000)
        return s3v_mem_read16(pc->vga, addr);
    if (addr < 0xfffe0000u && s3v_lfb_hit(pc->vga, addr))
        return s3v_lfb_read16(pc->vga, addr);
    return iomem_read8(o, addr) | (iomem_read8(o, addr + 1) << 8);
}

static void iomem_write16(void *o, uword addr, uint16_t val)
{
    PC *pc = o;
    if (addr < 0xc0000) {
        s3v_mem_write16(pc->vga, addr, val);
        return;
    }
    if (addr >= 0x100000 && s3v_lfb_hit(pc->vga, addr)) {
        s3v_lfb_write16(pc->vga, addr, val);
        return;
    }
    iomem_write8(o, addr, val);
    iomem_write8(o, addr + 1, val >> 8);
}

static uint32_t iomem_read32(void *o, uword addr)
{
    PC *pc = o;
    if (addr < 0xc0000)
        return s3v_mem_read32(pc->vga, addr);
    if (addr < 0xfffe0000u && s3v_lfb_hit(pc->vga, addr))
        return s3v_lfb_read32(pc->vga, addr);
    return iomem_read16(o, addr) | ((uint32_t)iomem_read16(o, addr + 2) << 16);
}

static void iomem_write32(void *o, uword addr, uint32_t val)
{
    PC *pc = o;
    if (addr < 0xc0000) {
        s3v_mem_write32(pc->vga, addr, val);
        return;
    }
    if (addr >= 0x100000 && s3v_lfb_hit(pc->vga, addr)) {
        s3v_lfb_write32(pc->vga, addr, val);
        return;
    }
    iomem_write16(o, addr, val & 0xffff);
    iomem_write16(o, addr + 2, val >> 16);
}

static bool iomem_write_string(void *o, uword addr, uint8_t *buf, int len)
{
    PC *pc = o;
    if (len <= 0)
        return false;
    if (addr < 0xc0000) {
        for (int i = 0; i < len; i++)
            s3v_mem_write8(pc->vga, addr + i, buf[i]);
        return true;
    }
    if (addr < 0x100000) {
        for (int i = 0; i < len; i++)
            rom_area_write8(pc, addr + i, buf[i]);
        return true;
    }
    if (s3v_lfb_hit(pc->vga, addr)) {
        for (int i = 0; i < len; i++)
            s3v_lfb_write8(pc->vga, addr + i, buf[i]);
        return true;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* first-boot CMOS: AMI defaults plus drives taken from the config      */

static const uint8_t ami_cmos_defaults[128] = {
    /* 00 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x01,0x95,0x26,0x02,0x00,0x80,0x00,0x00,
    /* 10 */ 0x00,0xbb,0x00,0xbc,0x0f,0x80,0x02,0x00,0x3c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 20 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3f,0x00,0x00, /* 2D bit5: boot A: first */
    /* 30 */ 0x00,0x3c,0x19,0x81,0x00,0x0f,0x01,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 40 */ 0x20,0x0a,0x10,0x1f,0x0f,0x00,0x0f,0x69,0x01,0x08,0x40,0x00,0x00,0x00,0x00,0x00,
    /* 50 */ 0x21,0x00,0x64,0x5b,0x88,0x65,0x00,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

static int floppy_type_for(const char *path)
{
    if (!path || !path[0])
        return 0;
    long size = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    switch (size) {
    case 368640: case 327680: case 184320: case 163840: return 1; /* 360K */
    case 1228800: return 2;  /* 1.2M */
    case 737280: return 3;   /* 720K */
    case 2949120: return 5;  /* 2.88M */
    default: return 4;       /* 1.44M */
    }
}

static void cmos_first_boot(PC *pc)
{
    uint8_t *c = rtc_nvram(pc->rtc);
    for (int i = 0x0e; i < 0x80; i++)
        if (i != 0x32)
            c[i] = ami_cmos_defaults[i];
    /* floppy drive A is always a 1.44M drive unless the image says else */
    int fa = pc->conf->fd[0] && pc->conf->fd[0][0] ? floppy_type_for(pc->conf->fd[0]) : 4;
    int fb = floppy_type_for(pc->conf->fd[1]);
    c[0x10] = (fa << 4) | fb;
    /* equipment: floppies, coprocessor, VGA */
    c[0x14] = 0x01 | (pc->conf->fpu ? 0x02 : 0) | (fb ? 0x40 : 0) | 0x0c;
    /* memory sizes */
    long ext = (pc->ram_size >> 10) - 1024;
    if (ext > 0xffff) ext = 0xffff;
    c[0x15] = 0x80; c[0x16] = 0x02;
    c[0x17] = c[0x30] = ext & 0xff;
    c[0x18] = c[0x31] = ext >> 8;
    /* hard disks as user type 47 */
    int cyl, hd, sec;
    c[0x12] = 0;
    if (ide_get_chs(pc->ide, 0, &cyl, &hd, &sec) == 0) {
        c[0x12] |= 0xf0;
        c[0x19] = 47;
        c[0x1b] = c[0x21] = cyl & 0xff;
        c[0x1c] = c[0x22] = cyl >> 8;
        c[0x1d] = hd;
        c[0x1e] = 0xff; c[0x1f] = 0xff;
        c[0x20] = 0xc0 | ((hd > 8) << 3);
        c[0x23] = sec;
    }
    if (ide_get_chs(pc->ide, 1, &cyl, &hd, &sec) == 0) {
        c[0x12] |= 0x0f;
        c[0x1a] = 47;
        c[0x24] = c[0x2a] = cyl & 0xff;
        c[0x25] = c[0x2b] = cyl >> 8;
        c[0x26] = hd;
        c[0x27] = 0xff; c[0x28] = 0xff;
        c[0x29] = 0xc0 | ((hd > 8) << 3);
        c[0x2c] = sec;
    }
    /* standard checksum 10h-2Dh at 2Eh/2Fh */
    unsigned sum = 0;
    for (int i = 0x10; i <= 0x2d; i++) sum += c[i];
    c[0x2e] = sum >> 8; c[0x2f] = sum & 0xff;
    /* AMI extended checksum: 34h-3Dh and 40h-7Fh at 3Eh/3Fh */
    sum = 0;
    for (int i = 0x34; i <= 0x3d; i++) sum += c[i];
    for (int i = 0x40; i <= 0x7f; i++) sum += c[i];
    c[0x3e] = sum >> 8; c[0x3f] = sum & 0xff;
    rtc_load_nvram(pc->rtc, c, 128);
}

/* keep the CMOS hard disk entries in step with the attached images:
 * a new image (or a different size) is set up as user type 47 */
static void cmos_sync_disks(PC *pc)
{
    uint8_t *c = rtc_nvram(pc->rtc);
    int changed = 0;
    for (int d = 0; d < 2; d++) {
        int cyl, hd, sec;
        int have = ide_get_chs(pc->ide, d, &cyl, &hd, &sec) == 0;
        int nib_shift = d ? 0 : 4;
        int type = (c[0x12] >> nib_shift) & 0x0f;
        int b = d ? 0x24 : 0x1b;          /* user type parameters */
        int ext = d ? 0x1a : 0x19;
        if (!have) {
            if (type == 0x0f && c[ext] == 47) {
                c[0x12] &= ~(0x0f << nib_shift);
                changed = 1;
            }
            continue;
        }
        int ccyl = c[b] | (c[b + 1] << 8);
        int chd = c[b + 2], csec = c[b + 8];
        if (type == 0x0f && c[ext] == 47 && ccyl == cyl && chd == hd && csec == sec)
            continue;
        if (type != 0 && !(type == 0x0f && c[ext] == 47))
            continue; /* a standard type chosen in setup: respect it */
        c[0x12] = (c[0x12] & ~(0x0f << nib_shift)) | (0x0f << nib_shift);
        c[ext] = 47;
        c[b] = c[b + 6] = cyl & 0xff;
        c[b + 1] = c[b + 7] = cyl >> 8;
        c[b + 2] = hd;
        c[b + 3] = 0xff; c[b + 4] = 0xff;
        c[b + 5] = 0xc0 | ((hd > 8) << 3);
        c[b + 8] = sec;
        changed = 1;
        plat_log("CMOS: disco %c: impostato tipo 47 (%d cilindri, %d testine, %d settori)\n",
                 'C' + d, cyl, hd, sec);
    }
    if (changed) {
        unsigned sum = 0;
        for (int i = 0x10; i <= 0x2d; i++) sum += c[i];
        c[0x2e] = sum >> 8; c[0x2f] = sum & 0xff;
        rtc_load_nvram(pc->rtc, c, 128);
    }
}

static void kbc_reset_cb(void *o)
{
    PC *pc = o;
    pc->reset_request = 1;
}

static void kbc_a20_cb(void *o, int val)
{
    PC *pc = o;
    cpui386_set_a20(pc->cpu, val);
}

void pc_save_nvram(PC *pc, int force)
{
    if (!pc->conf->nvram)
        return;
    if (rtc_take_dirty(pc->rtc))
        pc->nvram_pending = 1;
    if (pc->nvram_pending && (force || (get_uticks() - pc->nvram_save_t) > 2000000)) {
        plat_save_file(pc->conf->nvram, rtc_nvram(pc->rtc), 256);
        pc->nvram_pending = 0;
        pc->nvram_save_t = get_uticks();
    }
}

static void cpu_reset_only(PC *pc)
{
    cpui386_reset(pc->cpu);
}

void pc_reset(PC *pc)
{
    /* chipset back to power-on state: everything reads from ROM */
    memset(pc->ali, 0, sizeof(pc->ali));
    for (int i = 0; i < 8; i++)
        pc->blk_read_ram[i] = 1; /* force copy */
    shadow_recalc(pc);
    pc->port92 = 0;
    cpui386_set_a20(pc->cpu, 1);
    s3v_reset(pc->vga);
    cpu_reset_only(pc);
}

void typematic_step(PC *pc);
static void audio_idle(PC *pc);

void pc_step(PC *pc)
{
    if (pc->reset_request) {
        pc->reset_request = 0;
        cpu_reset_only(pc);
    }
    if (cpui386_is_shutdown(pc->cpu)) {
        plat_log("CPU shutdown (triple fault) -> reset\n");
        cpu_reset_only(pc);
    }

    i8254_update_irq(pc->pit);
    rtc_step(pc->rtc);
    kbd_step(pc->kbc);
    typematic_step(pc);
    fdc_step(pc->fdc);
    uart_step(pc->com1);
    i8257_dma_run(pc->dma);
    i8257_dma_run(pc->hdma);
    audio_idle(pc);

    int n = PC_STEP_COUNT;
    if (pc->speed_kips > 0) {
        /* throttle in small slices (about 100 us) so that programs which
           count loop iterations between two timer interrupts see an even
           speed */
        uint32_t now = get_uticks();
        uint32_t el = now - pc->speed_t0;
        uint64_t budget = (uint64_t)el * pc->speed_kips / 1000;
        if (pc->speed_icount >= budget)
            return;
        uint64_t behind = budget - pc->speed_icount;
        if (behind > (uint64_t)pc->speed_kips * 2) {
            /* more than 2 ms late (slow host or pause): do not catch up */
            pc->speed_t0 = now;
            pc->speed_icount = 0;
            behind = pc->speed_kips / 10;
        } else if (el > 1000000) {
            pc->speed_t0 = now;
            pc->speed_icount = 0;
        }
        n = pc->speed_kips / 10;
        if (n < 32) n = 32;
        if (n > PC_STEP_COUNT) n = PC_STEP_COUNT;
        if ((uint64_t)n > behind) n = behind < 32 ? 32 : (int)behind;
        pc->speed_icount += n;
    }
    cpui386_step(pc->cpu, n);
    pc_save_nvram(pc, 0);
}

#include "keymap_hid.h"

static void key_raw(PC *pc, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        ps2_queue(pc->kbd, b[i]);
}

void pc_key_event(PC *pc, int is_down, int keycode)
{
    if (keycode == KEY_PAUSE) {
        static const uint8_t seq[] = { 0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5 };
        if (is_down) key_raw(pc, seq, sizeof(seq));
        return;
    }
    if (keycode == KEY_PRINTSCREEN) {
        static const uint8_t dn[] = { 0xe0, 0x2a, 0xe0, 0x37 };
        static const uint8_t up[] = { 0xe0, 0xb7, 0xe0, 0xaa };
        key_raw(pc, is_down ? dn : up, 4);
        return;
    }
    ps2_put_keycode(pc->kbd, is_down, keycode);
    /* typematic repeat bookkeeping (USB keyboards do not repeat) */
    if (is_down) {
        int mod = keycode == 0x1d || keycode == 0x2a || keycode == 0x36 ||
                  keycode == 0x38 || keycode == 0xe01d || keycode == 0xe038 ||
                  keycode == 0x3a || keycode == 0x45 || keycode == 0x46;
        if (!mod) {
            pc->rep_key = keycode;
            pc->rep_time = get_uticks() + 500000;
        }
    } else if (keycode == pc->rep_key) {
        pc->rep_key = 0;
    }
}

void typematic_step(PC *pc)
{
    if (pc->rep_key && (int32_t)(get_uticks() - pc->rep_time) >= 0) {
        ps2_put_keycode(pc->kbd, 1, pc->rep_key);
        pc->rep_time = get_uticks() + 33333;
    }
}

void pc_mouse_event(PC *pc, int dx, int dy, int dz, int buttons)
{
    if (pc->conf->serial_mouse)
        uart_mouse_event(pc->com1, dx, dy, buttons);
    else
        ps2_mouse_event(pc->mouse, dx, dy, dz, buttons);
}

#define MIXER_BUF_LEN 4096
static void mix_audio(PC *pc, uint8_t *stream, int len);

void pc_audio_callback(PC *pc, uint8_t *stream, int len)
{
    pc->audio_ext_t = get_uticks();
    mix_audio(pc, stream, len);
}

/* without an audio output nobody pulls samples: consume them here at the
   real-time rate, otherwise Sound Blaster DMA never ends and programs that
   wait for its interrupt hang */
static void audio_idle(PC *pc)
{
    uint32_t now = get_uticks();
    if (!pc->sb16 || now - pc->audio_ext_t < 200000)
        return;
    uint32_t el = now - pc->audio_int_t;
    if (el < 10000)
        return;
    int frames = (int)((uint64_t)el * 44100 / 1000000);
    if (frames > 1024 || el > 100000)
        frames = 1024;
    static uint8_t scratch[1024 * 4];
    mix_audio(pc, scratch, frames * 4);
    pc->audio_int_t = now;
}

static void mix_audio(PC *pc, uint8_t *stream, int len)
{
    static uint8_t tmpbuf[MIXER_BUF_LEN];
    memset(stream, 0, len);
    if (pc->sb16) {
        if (len / 2 > MIXER_BUF_LEN)
            len = MIXER_BUF_LEN * 2;
        memset(tmpbuf, 0, MIXER_BUF_LEN);
        adlib_callback(pc->adlib, tmpbuf, len / 2); /* s16 mono */
        sb16_audio_callback(pc->sb16, stream, len);  /* s16 stereo */
        int16_t *d2 = (int16_t *)stream;
        int16_t *d1 = (int16_t *)tmpbuf;
        for (int i = 0; i < len / 2; i++) {
            int r = d2[i] + d1[i / 2];
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            d2[i] = r;
        }
    }
    if (pcspk_get_active_out(pc->spk)) {
        int n = len / 4;
        if (n > MIXER_BUF_LEN) n = MIXER_BUF_LEN;
        memset(tmpbuf, 0x80, n);
        pcspk_callback(pc->spk, tmpbuf, n);
        int16_t *d2 = (int16_t *)stream;
        for (int i = 0; i < n * 2; i++) {
            int r = d2[i] + (((int)tmpbuf[i / 2] - 0x80) << 5);
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            d2[i] = r;
        }
    }
}

PC *pc_new(const PCConfig *conf)
{
    PC *pc = calloc(1, sizeof(PC));
    CPU_CB *cb = NULL;
    pc->conf = conf;
    pc->ram_size = conf->mem_size;
#ifdef PC486_DEBUG_DE
    g_dbg_pc = pc;
#endif
    pc->ram = bigmalloc(conf->mem_size);
    memset(pc->ram, 0, conf->mem_size);

    pc->cpu = cpui386_new(conf->cpu_gen ? conf->cpu_gen : 4, (char *)pc->ram,
                          conf->mem_size, &cb);
    if (conf->fpu)
        cpui386_enable_fpu(pc->cpu);
    pc->speed_kips = conf->speed_kips;
    pc->speed_t0 = get_uticks();

    /* ROMs */
    memset(pc->rom, 0xff, sizeof(pc->rom));
    {
        uint8_t *tmp = malloc(0x20000);
        long n = plat_load_file(conf->bios, tmp, 0x20000);
        if (n <= 0) {
            plat_log("cannot load BIOS %s\n", conf->bios);
            free(tmp);
            return NULL;
        }
        /* BIOS is placed at the top of the 1MB space */
        memcpy(pc->rom + 0x40000 - n, tmp, n);
        if (conf->vga_bios && conf->vga_bios[0]) {
            n = plat_load_file(conf->vga_bios, tmp, 0x10000);
            if (n > 0)
                memcpy(pc->rom, tmp, n);
            else
                plat_log("cannot load VGA BIOS %s\n", conf->vga_bios);
        }
        free(tmp);
    }

    pc->pic = i8259_init(raise_irq_cb, pc->cpu);
    cb->pic = pc->pic;
    cb->pic_read_irq = read_irq_cb;

    pc->pit = i8254_init(0, pc->pic, set_irq_cb);
    pc->rtc = rtc_new(8, pc->pic, set_irq_cb);
    int have_nvram = 0;
    if (conf->nvram) {
        uint8_t nv[256];
        long n = plat_load_file(conf->nvram, nv, sizeof(nv));
        if (n >= 128) {
            rtc_load_nvram(pc->rtc, nv, n);
            have_nvram = 1;
        }
    }

    pc->dma = i8257_new((char *)pc->ram, pc->ram_size, 0x00, 0x80, 0x480, 0);
    pc->hdma = i8257_new((char *)pc->ram, pc->ram_size, 0xc0, 0x88, 0x488, 1);
    pc->fdc = fdc_new(6, 2, pc->pic, set_irq_cb, pc->dma);
    for (int i = 0; i < 2; i++)
        if (conf->fd[i] && conf->fd[i][0])
            if (fdc_insert(pc->fdc, i, conf->fd[i]) < 0)
                plat_log("cannot open floppy image %s\n", conf->fd[i]);

    pc->ide = ide_allocate(14, pc->pic, set_irq_cb);
    pc->ide2 = ide_allocate(15, pc->pic, set_irq_cb);
    for (int i = 0; i < 4; i++) {
        if (!conf->hd[i] || !conf->hd[i][0])
            continue;
        IDEIFState *bus = i < 2 ? pc->ide : pc->ide2;
        const char *base = strrchr(conf->hd[i], '/');
        base = base ? base + 1 : conf->hd[i];
        if (!conf->iscd[i] && (!strncmp(base, "sd:", 3) || !strncmp(base, "SD:", 3)) &&
            base[3] >= '1' && base[3] <= '4' && base[4] == 0) {
            /* a partition of the SD card used directly as hard disk */
            char msg[128];
            int r = ide_attach_sdpart(bus, i & 1, base[3] - '0', msg, sizeof(msg));
            plat_log("%s%s\n", r < 0 ? "errore: " : "", msg);
            continue;
        }
        int r = conf->iscd[i] ? ide_attach_cd(bus, i & 1, conf->hd[i])
                              : ide_attach(bus, i & 1, conf->hd[i]);
        if (r < 0)
            plat_log("cannot attach disk %s\n", conf->hd[i]);
    }

    pc->vga = s3v_new(conf->vram_size ? conf->vram_size : 4 << 20);
    if (conf->pci) {
        /* PCI bus + PCI BIOS extension ROM at C8000 */
        pc->has_pci = 1;
        pcibus_init(&pc->pcibus, pc->vga,
                    (void (*)(void *, uint32_t, int))s3v_set_pci_base);
        memcpy(pc->rom + 0x8000, pcibios_rom, sizeof(pcibios_rom));
        uint8_t sum = 0;
        for (unsigned i = 0; i < sizeof(pcibios_rom) - 1; i++)
            sum += pcibios_rom[i];
        pc->rom[0x8000 + sizeof(pcibios_rom) - 1] = (uint8_t)-sum;
    }
    if (!have_nvram)
        cmos_first_boot(pc);
    else
        cmos_sync_disks(pc);

    pc->kbc = i8042_init(&pc->kbd, &pc->mouse, 1, 12, pc->pic, set_irq_cb,
                         pc, kbc_reset_cb);
    i8042_set_a20_cb(pc->kbc, kbc_a20_cb);

    pc->spk = pcspk_init(pc->pit);
    if (conf->sound) {
        pc->adlib = adlib_new();
        pc->sb16 = sb16_new(0x220, 5, pc->dma, pc->hdma, pc->pic, set_irq_cb);
    }
    pc->com1 = uart_new(4, pc->pic, set_irq_cb, conf->serial_mouse);
    pc->com2 = uart_new(3, pc->pic, set_irq_cb, 0);

    cb->io = pc;
    cb->io_read8 = io_read8;
    cb->io_write8 = io_write8;
    cb->io_read16 = io_read16;
    cb->io_write16 = io_write16;
    cb->io_read32 = io_read32;
    cb->io_write32 = io_write32;
    cb->io_read_string = io_read_string;
    cb->io_write_string = io_write_string;
    cb->iomem = pc;
    cb->iomem_read8 = iomem_read8;
    cb->iomem_write8 = iomem_write8;
    cb->iomem_read16 = iomem_read16;
    cb->iomem_write16 = iomem_write16;
    cb->iomem_read32 = iomem_read32;
    cb->iomem_write32 = iomem_write32;
    cb->iomem_write_string = iomem_write_string;

    pc_reset(pc);
    return pc;
}
