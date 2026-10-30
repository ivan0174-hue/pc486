/*
 * pc486 - S3 ViRGE (86C325) 2D engine
 *
 * MMIO layout (offset inside the 64K MMIO window; the window is either the
 * "new MMIO" at linear base + 16MB or the "old MMIO" at A0000 when CR53
 * bit 3 is set):
 *   0000-7FFF  image transfer (host data for BitBLTs with IDS)
 *   8180-81FF  streams processor (stored, not emulated)
 *   8200-821F  memory port controller (stored)
 *   83B0-83DF  VGA registers
 *   8504       subsystem status / control
 *   850C       advanced function control
 *   A000-A1FF  colour pattern
 *   A4D4-A50C  BitBLT / rectangle fill registers
 *   A8D4-A97C  line registers
 *   ACD4-AD7C  polygon fill registers
 *   B0D4-B57C  3D registers (stored, not emulated)
 */
#include <stdlib.h>
#include <string.h>
#include "s3accel.h"

#define CMD_AE      (1u << 0)
#define CMD_HC      (1u << 1)
#define CMD_MS      (1u << 6)
#define CMD_IDS     (1u << 7)
#define CMD_MP      (1u << 8)
#define CMD_TP      (1u << 9)
#define CMD_XP      (1u << 25)
#define CMD_YP      (1u << 26)
#define CMD_3D      (1u << 31)

enum { OP_BITBLT = 0, OP_RECTFILL = 2, OP_LINE = 3, OP_POLY = 5, OP_NOP = 15 };

struct S3Accel {
    uint8_t *vram;
    uint32_t vram_size, vram_mask;
    const uint8_t *cr;
    void *vga;
    uint8_t (*vga_read)(void *vga, uint32_t port);
    void (*vga_write)(void *vga, uint32_t port, uint8_t val);

    /* register file (shared by all 2D command groups) */
    uint32_t src_base, dest_base;
    int clip_l, clip_r, clip_t, clip_b;
    uint32_t src_str, dest_str;
    uint64_t mono_pat;
    uint32_t pat_bg, pat_fg, src_bg, src_fg;
    uint32_t cmd_set;
    int r_width, r_height, rsrc_x, rsrc_y, rdest_x, rdest_y;
    /* line */
    int lxend0, lxend1;
    int32_t ldx, lxstart;
    int lystart, lycnt, ldir;
    /* polygon */
    int32_t prdx, prxstart, pldx, plxstart;
    int pystart, pycnt;
    uint8_t pattern[0x200];
    uint8_t regs[0x10000];   /* raw copy of everything written */

    /* BitBLT in progress waiting for host data */
    int busy;
    int bpp;                 /* bytes per pixel */
    int x, y, sx, sy, w_left, h_left;
    int xdir, ydir;
    uint8_t rop;
    /* host data bit queue */
    uint64_t hbits;
    int hcount;              /* valid bits in hbits */
    int line_bits;           /* bits consumed in the current line */

    uint32_t subsys_ctrl, adv_func;
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */

static inline int fmt_bpp(uint32_t cmd)
{
    switch ((cmd >> 2) & 7) {
    case 1: return 2;
    case 2: return 3;
    default: return 1;
    }
}

static inline uint32_t rd_pix(S3Accel *a, uint32_t addr, int bpp)
{
    uint32_t m = a->vram_mask;
    switch (bpp) {
    case 1: return a->vram[addr & m];
    case 2: return a->vram[addr & m] | (a->vram[(addr + 1) & m] << 8);
    default: return a->vram[addr & m] | (a->vram[(addr + 1) & m] << 8) |
                    (a->vram[(addr + 2) & m] << 16);
    }
}

static inline void wr_pix(S3Accel *a, uint32_t addr, int bpp, uint32_t v)
{
    uint32_t m = a->vram_mask;
    a->vram[addr & m] = v;
    if (bpp >= 2) a->vram[(addr + 1) & m] = v >> 8;
    if (bpp >= 3) a->vram[(addr + 2) & m] = v >> 16;
}

static uint32_t rop3(uint8_t rop, uint32_t p, uint32_t s, uint32_t d)
{
    switch (rop) {
    case 0x00: return 0;
    case 0xff: return 0xffffffff;
    case 0xcc: return s;
    case 0xf0: return p;
    case 0xaa: return d;
    case 0x55: return ~d;
    case 0x33: return ~s;
    case 0x0f: return ~p;
    case 0x66: return s ^ d;
    case 0x5a: return p ^ d;
    case 0x88: return s & d;
    case 0xee: return s | d;
    case 0xbb: return ~s | d;
    case 0x44: return s & ~d;
    case 0x22: return ~s & d;
    case 0xa0: return p & d;
    case 0xfa: return p | d;
    case 0xc0: return p & s;
    case 0xb8: return ((p ^ d) & s) ^ p;
    case 0xe2: return ((p ^ d) & s) ^ d;
    }
    uint32_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (!(rop & (1 << i)))
            continue;
        uint32_t t = (i & 4) ? p : ~p;
        t &= (i & 2) ? s : ~s;
        t &= (i & 1) ? d : ~d;
        r |= t;
    }
    return r;
}

static inline uint32_t colour_pattern(S3Accel *a, int x, int y, int bpp)
{
    int i = ((y & 7) << 3) | (x & 7);
    switch (bpp) {
    case 1: return a->pattern[i];
    case 2: return a->pattern[i * 2] | (a->pattern[i * 2 + 1] << 8);
    default: return a->pattern[i * 3] | (a->pattern[i * 3 + 1] << 8) |
                    (a->pattern[i * 3 + 2] << 16);
    }
}

static inline uint32_t get_pattern(S3Accel *a, int x, int y, int bpp)
{
    if (a->cmd_set & CMD_MP) {
        int bit = (a->mono_pat >> (((y & 7) << 3) + (7 - (x & 7)))) & 1;
        return bit ? a->pat_fg : a->pat_bg;
    }
    /* fills, lines and polygons use the pattern foreground colour;
     * only BitBLT uses the colour pattern registers */
    if (((a->cmd_set >> 27) & 15) != OP_BITBLT)
        return a->pat_fg;
    return colour_pattern(a, x, y, bpp);
}

static inline int clipped(S3Accel *a, int x, int y)
{
    if (!(a->cmd_set & CMD_HC))
        return 0;
    return x < a->clip_l || x > a->clip_r || y < a->clip_t || y > a->clip_b;
}

/* writes one pixel through the ROP (src already resolved) */
static inline void put_pixel(S3Accel *a, int x, int y, uint32_t src, int bpp)
{
    if (clipped(a, x, y) || x < 0 || y < 0)
        return;
    uint32_t addr = a->dest_base + y * a->dest_str + x * bpp;
    uint32_t d = rd_pix(a, addr, bpp);
    uint32_t p = get_pattern(a, x, y, bpp);
    uint32_t r = rop3(a->rop, p, src, d);
    wr_pix(a, addr, bpp, r);
}

/* ------------------------------------------------------------------ */
/* BitBLT / rectangle fill                                            */

static void blt_next_line(S3Accel *a)
{
    a->w_left = a->r_width;
    a->x = a->rdest_x;
    a->sx = a->rsrc_x;
    a->y += a->ydir;
    a->sy += a->ydir;
    a->h_left--;
}

/* one pixel of a BitBLT with a given source value; returns 0 when done */
static int blt_pixel(S3Accel *a, uint32_t src, int src_is_mono_bit)
{
    int bpp = a->bpp;
    if (src_is_mono_bit >= 0) {
        if (!src_is_mono_bit && (a->cmd_set & CMD_TP))
            goto skip;
        src = src_is_mono_bit ? a->src_fg : a->src_bg;
    }
    put_pixel(a, a->x, a->y, src, bpp);
skip:
    a->x += a->xdir;
    a->sx += a->xdir;
    if (--a->w_left < 0) {
        blt_next_line(a);
        if (a->h_left <= 0) {
            a->busy = 0;
            return 0;
        }
        return 2; /* end of line */
    }
    return 1;
}

static void blt_start(S3Accel *a)
{
    int op = (a->cmd_set >> 27) & 15;
    a->bpp = fmt_bpp(a->cmd_set);
    a->rop = (a->cmd_set >> 17) & 0xff;
    a->xdir = (a->cmd_set & CMD_XP) ? 1 : -1;
    a->ydir = (a->cmd_set & CMD_YP) ? 1 : -1;
    a->x = a->rdest_x;
    a->y = a->rdest_y;
    a->sx = a->rsrc_x;
    a->sy = a->rsrc_y;
    a->w_left = a->r_width;
    a->h_left = a->r_height;
    a->hbits = 0;
    a->hcount = 0;
    a->line_bits = 0;
    a->busy = 0;
    if (a->h_left <= 0)
        return;

    if (op == OP_RECTFILL) {
        /* source is not used: the fill colour comes from the pattern */
        for (;;) {
            if (!blt_pixel(a, 0, -1))
                break;
        }
        return;
    }

    if (a->cmd_set & CMD_IDS) {
        a->busy = 1; /* wait for data from the CPU */
        return;
    }

    /* screen to screen */
    int bpp = a->bpp;
    for (;;) {
        int r;
        if (a->cmd_set & CMD_MS) {
            /* monochrome source in video memory: 1 bit per pixel */
            uint32_t addr = a->src_base + a->sy * a->src_str + (a->sx >> 3);
            int bit = (a->vram[addr & a->vram_mask] >> (7 - (a->sx & 7))) & 1;
            r = blt_pixel(a, 0, bit);
        } else {
            uint32_t addr = a->src_base + a->sy * a->src_str + a->sx * bpp;
            r = blt_pixel(a, rd_pix(a, addr, bpp), -1);
        }
        if (!r)
            break;
    }
}

/* data written by the CPU into the image transfer area */
static void blt_host_data(S3Accel *a, uint32_t val)
{
    if (!a->busy)
        return;
    int mono = (a->cmd_set & CMD_MS) != 0;
    if (mono) {
        /* bytes in memory order, bits MSB first */
        val = ((val & 0xff) << 24) | ((val & 0xff00) << 8) |
              ((val >> 8) & 0xff00) | (val >> 24);
        a->hbits = (a->hbits << 32) | val;
    } else {
        /* pixels are little endian: keep a little-endian bit queue */
        a->hbits |= (uint64_t)val << a->hcount;
    }
    a->hcount += 32;

    int align;
    switch ((a->cmd_set >> 10) & 3) {
    case 0: align = 8; break;
    case 1: align = 16; break;
    default: align = 32; break;
    }
    int pbits = mono ? 1 : a->bpp * 8;

    while (a->busy && a->hcount >= pbits) {
        int r;
        if (mono) {
            int bit = (a->hbits >> (a->hcount - 1)) & 1;
            a->hcount--;
            r = blt_pixel(a, 0, bit);
        } else {
            uint32_t pix = (uint32_t)(a->hbits & ((1ull << pbits) - 1));
            a->hbits >>= pbits;
            a->hcount -= pbits;
            r = blt_pixel(a, pix, -1);
        }
        a->line_bits += pbits;
        if (r == 2 || r == 0) {
            /* lines start on an aligned boundary of the data stream */
            int pad = (align - (a->line_bits % align)) % align;
            if (pad > a->hcount)
                pad = a->hcount; /* rest comes with the next dword */
            if (mono) {
                a->hcount -= pad;
            } else {
                a->hbits >>= pad;
                a->hcount -= pad;
            }
            a->line_bits = 0;
            /* if the padding spills into the next dword, remember it */
            if (a->hcount == 0 && pad < (align - 0) && align == 32) {
                /* dword aligned: the rest of this dword is discarded */
            }
        }
        if (mono)
            a->hbits &= a->hcount ? ((a->hcount >= 64) ? ~0ull : ((1ull << a->hcount) - 1)) : 0;
    }
    if (!a->busy) {
        a->hbits = 0;
        a->hcount = 0;
    }
}

/* ------------------------------------------------------------------ */
/* line and polygon                                                   */

static void hspan(S3Accel *a, int x1, int x2, int y)
{
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    for (int x = x1; x <= x2; x++)
        put_pixel(a, x, y, a->pat_fg, a->bpp);
}

static void line_draw(S3Accel *a)
{
    a->bpp = fmt_bpp(a->cmd_set);
    a->rop = (a->cmd_set >> 17) & 0xff;
    /* lines are drawn from the bottom (LYSTART) upwards, one horizontal
     * span per scanline; x positions are 12.20 fixed point */
    int32_t x = a->lxstart;
    int y = a->lystart;
    int n = a->lycnt;
    for (int i = 0; i < n; i++, y--) {
        int xs = x >> 20;
        int32_t xn = x + a->ldx;
        int xe = (xn >> 20) - (a->ldir ? 1 : -1);
        if (i == 0) xs = a->lxend0;
        if (i == n - 1) xe = a->lxend1;
        if (a->ldir) {
            if (xe < xs) xe = xs;
        } else {
            if (xe > xs) xe = xs;
        }
        hspan(a, xs, xe, y);
        x = xn;
    }
}

static void poly_fill(S3Accel *a)
{
    a->bpp = fmt_bpp(a->cmd_set);
    a->rop = (a->cmd_set >> 17) & 0xff;
    int32_t xl = a->plxstart, xr = a->prxstart;
    int y = a->pystart;
    for (int i = 0; i < a->pycnt; i++, y--) {
        hspan(a, xl >> 20, (xr >> 20), y);
        xl += a->pldx;
        xr += a->prdx;
    }
}

/* ------------------------------------------------------------------ */
/* registers                                                          */

static void start_cmd(S3Accel *a)
{
    if (a->cmd_set & CMD_3D)
        return; /* 3D engine not emulated */
    switch ((a->cmd_set >> 27) & 15) {
    case OP_BITBLT:
    case OP_RECTFILL:
        blt_start(a);
        break;
    case OP_LINE:
        line_draw(a);
        break;
    case OP_POLY:
        poly_fill(a);
        break;
    }
}

static void reg_write32(S3Accel *a, uint32_t r, uint32_t val)
{
    *(uint32_t *)&a->regs[r & 0xfffc] = val;

    if (r >= 0xa000 && r < 0xa200) {
        memcpy(&a->pattern[r & 0x1fc], &val, 4);
        return;
    }
    if (r >= 0xa400 && r < 0xb000) {
        /* the three register groups share layout, 0x400 apart */
        int grp = (r - 0xa400) >> 10; /* 0 blt, 1 line, 2 poly */
        uint32_t o = r - 0x400 * grp;
        switch (o) {
        case 0xa4d4: a->src_base = val & 0x3ffff8; return;
        case 0xa4d8: a->dest_base = val & 0x3ffff8; return;
        case 0xa4dc: a->clip_l = (val >> 16) & 0x7ff; a->clip_r = val & 0x7ff; return;
        case 0xa4e0: a->clip_t = (val >> 16) & 0x7ff; a->clip_b = val & 0x7ff; return;
        case 0xa4e4: a->src_str = val & 0xff8; a->dest_str = (val >> 16) & 0xff8; return;
        case 0xa4e8: a->mono_pat = (a->mono_pat & 0xffffffff00000000ull) | val; return;
        case 0xa4ec: a->mono_pat = (a->mono_pat & 0xffffffffull) | ((uint64_t)val << 32); return;
        case 0xa4f0: a->pat_bg = val; return;
        case 0xa4f4: a->pat_fg = val; return;
        case 0xa4f8: a->src_bg = val; return;
        case 0xa4fc: a->src_fg = val; return;
        case 0xa500:
            a->cmd_set = val;
            if (!(val & CMD_AE))
                start_cmd(a);
            return;
        }
        if (grp == 0) {
            switch (r) {
            case 0xa504: a->r_width = (val >> 16) & 0x7ff; a->r_height = val & 0x7ff; return;
            case 0xa508: a->rsrc_x = (val >> 16) & 0x7ff; a->rsrc_y = val & 0x7ff; return;
            case 0xa50c:
                a->rdest_x = (val >> 16) & 0x7ff;
                a->rdest_y = val & 0x7ff;
                if (a->cmd_set & CMD_AE)
                    start_cmd(a);
                return;
            }
        } else if (grp == 1) {
            switch (r) {
            case 0xa96c: a->lxend0 = (val >> 16) & 0x7ff; a->lxend1 = val & 0x7ff; return;
            case 0xa970: a->ldx = (int32_t)val; return;
            case 0xa974: a->lxstart = (int32_t)val; return;
            case 0xa978: a->lystart = val & 0x7ff; return;
            case 0xa97c:
                a->lycnt = val & 0x7ff;
                a->ldir = (val >> 31) & 1;
                if (a->cmd_set & CMD_AE)
                    start_cmd(a);
                return;
            }
        } else {
            switch (r) {
            case 0xad68: a->prdx = (int32_t)val; return;
            case 0xad6c: a->prxstart = (int32_t)val; return;
            case 0xad70: a->pldx = (int32_t)val; return;
            case 0xad74: a->plxstart = (int32_t)val; return;
            case 0xad78: a->pystart = val & 0x7ff; return;
            case 0xad7c:
                a->pycnt = val & 0x7ff;
                if (a->cmd_set & CMD_AE)
                    start_cmd(a);
                return;
            }
        }
        return;
    }
    switch (r) {
    case 0x8504: a->subsys_ctrl = val; return;
    case 0x850c: a->adv_func = val; return;
    }
}

static uint32_t reg_read32(S3Accel *a, uint32_t r)
{
    switch (r) {
    case 0x8504:
        /* bit 13: engine idle, bits 12-8: free command FIFO slots */
        return (a->busy ? 0 : (1u << 13)) | (0x1f << 8);
    case 0x850c:
        return a->adv_func;
    }
    return *(uint32_t *)&a->regs[r & 0xfffc];
}

/* ------------------------------------------------------------------ */
/* public interface                                                   */

bool s3a_mmio_hit_legacy(S3Accel *a, uint32_t addr)
{
    return (a->cr[0x53] & 0x08) && addr >= 0xa0000 && addr < 0xb0000;
}

static inline uint32_t reg_of(uint32_t addr)
{
    return addr & 0xffff;
}

static inline int is_vga(uint32_t r)
{
    return r >= 0x83b0 && r <= 0x83df;
}

void s3a_mmio_write32(S3Accel *a, uint32_t addr, uint32_t val)
{
    uint32_t r = reg_of(addr);
    if (r < 0x8000) {
        blt_host_data(a, val);
        return;
    }
    if (is_vga(r)) {
        for (int i = 0; i < 4; i++)
            a->vga_write(a->vga, 0x3b0 + ((r + i) & 0x3f), val >> (i * 8));
        return;
    }
    reg_write32(a, r & ~3u, val);
}

void s3a_mmio_write16(S3Accel *a, uint32_t addr, uint16_t val)
{
    uint32_t r = reg_of(addr);
    if (r < 0x8000) {
        /* 16-bit host data: accumulate into dwords */
        static uint32_t pending;
        static int have;
        if (!have) { pending = val; have = 1; }
        else { blt_host_data(a, pending | ((uint32_t)val << 16)); have = 0; }
        return;
    }
    if (is_vga(r)) {
        a->vga_write(a->vga, 0x3b0 + (r & 0x3f), val);
        a->vga_write(a->vga, 0x3b0 + ((r + 1) & 0x3f), val >> 8);
        return;
    }
    uint32_t cur = *(uint32_t *)&a->regs[r & 0xfffc];
    int sh = (r & 2) * 8;
    cur = (cur & ~(0xffffu << sh)) | ((uint32_t)val << sh);
    reg_write32(a, r & ~3u, cur);
}

void s3a_mmio_write8(S3Accel *a, uint32_t addr, uint8_t val)
{
    uint32_t r = reg_of(addr);
    if (r < 0x8000) {
        static uint32_t pending;
        static int have;
        pending |= (uint32_t)val << (have * 8);
        if (++have == 4) { blt_host_data(a, pending); pending = 0; have = 0; }
        return;
    }
    if (is_vga(r)) {
        a->vga_write(a->vga, 0x3b0 + (r & 0x3f), val);
        return;
    }
    uint32_t cur = *(uint32_t *)&a->regs[r & 0xfffc];
    int sh = (r & 3) * 8;
    cur = (cur & ~(0xffu << sh)) | ((uint32_t)val << sh);
    reg_write32(a, r & ~3u, cur);
}

uint32_t s3a_mmio_read32(S3Accel *a, uint32_t addr)
{
    uint32_t r = reg_of(addr);
    if (r < 0x8000)
        return 0xffffffff;
    if (is_vga(r)) {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++)
            v |= (uint32_t)a->vga_read(a->vga, 0x3b0 + ((r + i) & 0x3f)) << (i * 8);
        return v;
    }
    return reg_read32(a, r & ~3u);
}

uint16_t s3a_mmio_read16(S3Accel *a, uint32_t addr)
{
    uint32_t r = reg_of(addr);
    if (is_vga(r))
        return a->vga_read(a->vga, 0x3b0 + (r & 0x3f)) |
               (a->vga_read(a->vga, 0x3b0 + ((r + 1) & 0x3f)) << 8);
    return s3a_mmio_read32(a, addr & ~3u) >> ((addr & 2) * 8);
}

uint8_t s3a_mmio_read8(S3Accel *a, uint32_t addr)
{
    uint32_t r = reg_of(addr);
    if (is_vga(r))
        return a->vga_read(a->vga, 0x3b0 + (r & 0x3f));
    return s3a_mmio_read32(a, addr & ~3u) >> ((addr & 3) * 8);
}

void s3a_reset(S3Accel *a)
{
    uint8_t *vram = a->vram;
    uint32_t size = a->vram_size;
    const uint8_t *cr = a->cr;
    void *vga = a->vga;
    uint8_t (*rd)(void *, uint32_t) = a->vga_read;
    void (*wr)(void *, uint32_t, uint8_t) = a->vga_write;
    memset(a, 0, sizeof(*a));
    a->vram = vram;
    a->vram_size = size;
    a->vram_mask = size - 1;
    a->cr = cr;
    a->vga = vga;
    a->vga_read = rd;
    a->vga_write = wr;
}

S3Accel *s3a_new(uint8_t *vram, uint32_t vram_size, const uint8_t *cr, void *vga,
                 uint8_t (*vga_read)(void *, uint32_t),
                 void (*vga_write)(void *, uint32_t, uint8_t))
{
    S3Accel *a = calloc(1, sizeof(*a));
    a->vram = vram;
    a->vram_size = vram_size;
    a->cr = cr;
    a->vga = vga;
    a->vga_read = vga_read;
    a->vga_write = vga_write;
    s3a_reset(a);
    return a;
}

int s3a_busy(S3Accel *a)
{
    return a->busy;
}
