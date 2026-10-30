/*
 * pc486 - VGA core + S3 ViRGE (86C325) extensions
 *
 * Written for pc486. Video memory is kept in "plane interleaved" layout:
 * byte (P * 4 + n) holds plane n of planar offset P. In S3 enhanced
 * (linear/packed) modes the same memory is simply addressed as bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s3virge.h"
#include "s3accel.h"

uint32_t get_uticks(void);

struct S3VState {
    uint8_t *vram;
    uint32_t vram_size, vram_mask;

    uint8_t misc, fcr;
    uint8_t sr_idx, sr[256];
    uint8_t cr_idx, cr[256];
    uint8_t gr_idx, gr[16];
    uint8_t ar_idx, ar[32];
    int ar_ff;
    uint8_t dac_mask, dac_state, dac_ridx, dac_widx, dac_sub, dac_rsub;
    uint8_t dac_tmp[3];
    uint8_t pal[768];
    uint32_t pal32[256];
    uint32_t latch;
    uint32_t bank; /* 64K units (chain4/enhanced), 16K planar units otherwise */

    uint8_t hwc_fg[4], hwc_bg[4];
    int hwc_fg_idx, hwc_bg_idx;

    uint32_t t0;
    uint32_t frame_us, vret_us;
    uint32_t st1_toggle;

    S3Accel *accel;
    char desc[96];
};

static const uint32_t mask16[16] = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff,
    0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff,
    0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */

static inline bool enhanced(S3VState *s)
{
    /* CR31 bit 3: enhanced (linear packed) memory mapping */
    return (s->cr[0x31] & 0x08) != 0;
}

static void update_bank(S3VState *s)
{
    /* nothing to precompute; bank kept in s->bank */
}

static uint32_t c6to8(uint8_t v)
{
    v &= 0x3f;
    return (v << 2) | (v >> 4);
}

static void update_pal32(S3VState *s, int i)
{
    s->pal32[i] = (c6to8(s->pal[i * 3]) << 16) |
                  (c6to8(s->pal[i * 3 + 1]) << 8) |
                  c6to8(s->pal[i * 3 + 2]);
}

/* ------------------------------------------------------------------ */
/* CRTC                                                               */

static void cr_write(S3VState *s, uint8_t idx, uint8_t val)
{
    /* standard write protect of CR0-CR7 */
    if (idx <= 7 && (s->cr[0x11] & 0x80)) {
        if (idx == 7)
            s->cr[7] = (s->cr[7] & ~0x10) | (val & 0x10);
        return;
    }
    /* S3 register locks */
    if (idx >= 0x30 && idx <= 0x3f && idx != 0x38 && idx != 0x39 &&
        s->cr[0x38] != 0x48)
        return;
    if (idx >= 0x40 && (s->cr[0x39] & 0xf0) != 0xa0)
        return;

    switch (idx) {
    case 0x2d: case 0x2e: case 0x2f: case 0x30: /* chip id, read only */
    case 0x36: case 0x37: /* configuration straps */
        return;
    case 0x45:
        s->cr[idx] = val;
        return;
    case 0x4a:
        s->hwc_fg[s->hwc_fg_idx & 3] = val;
        s->hwc_fg_idx++;
        s->cr[idx] = val;
        return;
    case 0x4b:
        s->hwc_bg[s->hwc_bg_idx & 3] = val;
        s->hwc_bg_idx++;
        s->cr[idx] = val;
        return;
    case 0x35:
        s->cr[idx] = val;
        s->bank = (s->bank & 0x70) | (val & 0x0f);
        return;
    case 0x51:
        s->cr[idx] = val;
        s->bank = (s->bank & 0x4f) | ((val & 0x0c) << 2);
        return;
    case 0x6a:
        s->cr[idx] = val;
        s->bank = val & 0x7f;
        return;
    }
    s->cr[idx] = val;
}

static uint8_t cr_read(S3VState *s, uint8_t idx)
{
    switch (idx) {
    case 0x24: return s->ar_ff ? 0x80 : 0;
    case 0x26: return s->ar_idx;
    case 0x45:
        s->hwc_fg_idx = s->hwc_bg_idx = 0;
        return s->cr[0x45];
    case 0x22: return (s->latch >> ((s->gr[4] & 3) * 8)) & 0xff;
    }
    if (idx >= 0x40 && (s->cr[0x39] & 0xf0) != 0xa0 && idx != 0x6a)
        return 0xff;
    return s->cr[idx];
}

/* ------------------------------------------------------------------ */
/* timing                                                             */

static uint32_t vdisp_lines(S3VState *s)
{
    uint32_t v = s->cr[0x12] | ((s->cr[7] & 2) << 7) | ((s->cr[7] & 0x40) << 3) |
                 ((s->cr[0x5e] & 2) << 9);
    return v + 1;
}

static void update_timing(S3VState *s)
{
    uint32_t v = vdisp_lines(s);
    s->frame_us = (v <= 400) ? 14286 : 16667;
    s->vret_us = 1000;
}

void s3v_step(S3VState *s)
{
}

static uint8_t read_st1(S3VState *s)
{
    uint32_t now = get_uticks();
    uint32_t pos = (now - s->t0) % s->frame_us;
    uint8_t v = 0;
    if (pos >= s->frame_us - s->vret_us)
        v |= 0x09;
    else {
        /* horizontal blank toggles on every read */
        s->st1_toggle ^= 1;
        if (s->st1_toggle)
            v |= 0x01;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                */

uint8_t s3v_io_read(S3VState *s, uint32_t port)
{
    if ((port >= 0x3b0 && port <= 0x3bf && (s->misc & 1)) ||
        (port >= 0x3d0 && port <= 0x3df && !(s->misc & 1)))
        return 0xff;

    switch (port) {
    case 0x3c0:
        return s->ar_idx;
    case 0x3c1: {
        int i = s->ar_idx & 0x1f;
        return i < 0x15 ? s->ar[i] : 0;
    }
    case 0x3c2: {
        /* monitor sense: colour monitor connected */
        int sum = s->pal[0] + s->pal[1] + s->pal[2];
        return sum >= 0x4e ? 0x00 : 0x10;
    }
    case 0x3c3:
        return 1;
    case 0x3c4:
        return s->sr_idx;
    case 0x3c5:
        if (s->sr_idx >= 0x09 && s->sr[8] != 0x06)
            return 0xff;
        return s->sr[s->sr_idx];
    case 0x3c6:
        return s->dac_mask;
    case 0x3c7:
        return s->dac_state;
    case 0x3c8:
        return s->dac_widx;
    case 0x3c9: {
        uint8_t v = s->pal[s->dac_ridx * 3 + s->dac_rsub];
        if (++s->dac_rsub == 3) {
            s->dac_rsub = 0;
            s->dac_ridx++;
        }
        return v;
    }
    case 0x3ca:
        return s->fcr;
    case 0x3cc:
        return s->misc;
    case 0x3ce:
        return s->gr_idx;
    case 0x3cf:
        return s->gr[s->gr_idx & 0x0f];
    case 0x3b4: case 0x3d4:
        return s->cr_idx;
    case 0x3b5: case 0x3d5:
        return cr_read(s, s->cr_idx);
    case 0x3ba: case 0x3da:
        s->ar_ff = 0;
        return read_st1(s);
    }
    return 0xff;
}

void s3v_io_write(S3VState *s, uint32_t port, uint8_t val)
{
    if ((port >= 0x3b0 && port <= 0x3bf && (s->misc & 1)) ||
        (port >= 0x3d0 && port <= 0x3df && !(s->misc & 1)))
        return;

    switch (port) {
    case 0x3b4: case 0x3d4:
        s->cr_idx = val;
        break;
    case 0x3b5: case 0x3d5:
        cr_write(s, s->cr_idx, val);
        if (s->cr_idx == 0x12 || s->cr_idx == 7 || s->cr_idx == 0x5e)
            update_timing(s);
        break;
    case 0x3ba: case 0x3da:
        s->fcr = val;
        break;
    case 0x3c0:
        if (!s->ar_ff) {
            s->ar_idx = val & 0x3f;
        } else {
            int i = s->ar_idx & 0x1f;
            if (i <= 0x0f)
                s->ar[i] = val & 0x3f;
            else if (i < 0x15)
                s->ar[i] = val;
        }
        s->ar_ff ^= 1;
        break;
    case 0x3c2:
        s->misc = val;
        break;
    case 0x3c4:
        s->sr_idx = val;
        break;
    case 0x3c5:
        if (s->sr_idx >= 0x09 && s->sr[8] != 0x06)
            break;
        s->sr[s->sr_idx] = val;
        break;
    case 0x3c6:
        s->dac_mask = val;
        break;
    case 0x3c7:
        s->dac_ridx = val;
        s->dac_rsub = 0;
        s->dac_state = 3;
        break;
    case 0x3c8:
        s->dac_widx = val;
        s->dac_sub = 0;
        s->dac_state = 0;
        break;
    case 0x3c9:
        s->dac_tmp[s->dac_sub++] = val & 0x3f;
        if (s->dac_sub == 3) {
            memcpy(&s->pal[s->dac_widx * 3], s->dac_tmp, 3);
            update_pal32(s, s->dac_widx);
            s->dac_widx++;
            s->dac_sub = 0;
        }
        break;
    case 0x3ce:
        s->gr_idx = val;
        break;
    case 0x3cf:
        s->gr[s->gr_idx & 0x0f] = val;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* memory                                                             */

/* map a legacy window address to an offset; -1 if not decoded */
static inline int32_t win_off(S3VState *s, uint32_t addr)
{
    addr -= 0xa0000;
    switch ((s->gr[6] >> 2) & 3) {
    case 0:
        return addr;
    case 1:
        return addr < 0x10000 ? (int32_t)addr : -1;
    case 2:
        return (addr >= 0x10000 && addr < 0x18000) ? (int32_t)(addr - 0x10000) : -1;
    default:
        return addr >= 0x18000 ? (int32_t)(addr - 0x18000) : -1;
    }
}

uint8_t s3v_mem_read8(S3VState *s, uint32_t addr)
{
    if (s->accel && s3a_mmio_hit_legacy(s->accel, addr))
        return s3a_mmio_read8(s->accel, addr);
    int32_t off = win_off(s, addr);
    if (off < 0)
        return 0xff;
    if (enhanced(s)) {
        return s->vram[((s->bank << 16) + off) & s->vram_mask];
    }
    if (s->sr[4] & 0x08) {
        /* chain 4 */
        uint32_t a = (s->bank << 16) + off;
        uint32_t p = a & ~3u;
        s->latch = *(uint32_t *)&s->vram[(p << 2) & s->vram_mask];
        return s->vram[((p << 2) | (a & 3)) & s->vram_mask];
    }
    uint32_t p = (s->bank << 14) + off;
    if (!(s->sr[4] & 0x04)) {
        /* odd/even */
        int plane = (s->gr[4] & 2) | (p & 1);
        p &= ~1u;
        s->latch = *(uint32_t *)&s->vram[(p << 2) & s->vram_mask];
        return s->vram[((p << 2) | plane) & s->vram_mask];
    }
    s->latch = *(uint32_t *)&s->vram[(p << 2) & s->vram_mask];
    if (!(s->gr[5] & 0x08)) {
        return (s->latch >> ((s->gr[4] & 3) * 8)) & 0xff;
    } else {
        uint32_t r = (s->latch ^ mask16[s->gr[2] & 0x0f]) & mask16[s->gr[7] & 0x0f];
        r |= r >> 16;
        r |= r >> 8;
        return (~r) & 0xff;
    }
}

void s3v_mem_write8(S3VState *s, uint32_t addr, uint8_t val)
{
    if (s->accel && s3a_mmio_hit_legacy(s->accel, addr)) {
        s3a_mmio_write8(s->accel, addr, val);
        return;
    }
    int32_t off = win_off(s, addr);
    if (off < 0)
        return;
    if (enhanced(s)) {
        s->vram[((s->bank << 16) + off) & s->vram_mask] = val;
        return;
    }
    uint32_t mapmask = s->sr[2] & 0x0f;
    if (s->sr[4] & 0x08) {
        uint32_t a = (s->bank << 16) + off;
        int plane = a & 3;
        if (mapmask & (1 << plane))
            s->vram[(((a & ~3u) << 2) | plane) & s->vram_mask] = val;
        return;
    }
    uint32_t p = (s->bank << 14) + off;
    if (!(s->sr[4] & 0x04)) {
        int plane = p & 1;
        if (s->misc & 0x20)
            ; /* page select: ignored */
        if (mapmask & (1 << plane)) {
            p &= ~1u;
            s->vram[((p << 2) | plane) & s->vram_mask] = val;
            /* odd/even into planes 2/3 as well when enabled (font load
             * uses planar mode, so this is only for completeness) */
            if (mapmask & (4 << plane))
                s->vram[((p << 2) | (plane + 2)) & s->vram_mask] = val;
        }
        return;
    }

    /* planar write */
    uint32_t v, bit_mask, set_mask;
    int b;
    switch (s->gr[5] & 3) {
    default:
    case 0:
        b = s->gr[3] & 7;
        val = ((val >> b) | (val << (8 - b))) & 0xff;
        v = val * 0x01010101u;
        set_mask = mask16[s->gr[1] & 0x0f];
        v = (v & ~set_mask) | (mask16[s->gr[0] & 0x0f] & set_mask);
        bit_mask = s->gr[8];
        break;
    case 1:
        v = s->latch;
        goto do_write;
    case 2:
        v = mask16[val & 0x0f];
        bit_mask = s->gr[8];
        break;
    case 3:
        b = s->gr[3] & 7;
        val = ((val >> b) | (val << (8 - b))) & 0xff;
        bit_mask = s->gr[8] & val;
        v = mask16[s->gr[0] & 0x0f];
        break;
    }
    switch ((s->gr[3] >> 3) & 3) {
    case 1: v &= s->latch; break;
    case 2: v |= s->latch; break;
    case 3: v ^= s->latch; break;
    }
    bit_mask |= bit_mask << 8;
    bit_mask |= bit_mask << 16;
    v = (v & bit_mask) | (s->latch & ~bit_mask);
do_write:
    {
        uint32_t wm = mask16[mapmask];
        uint32_t *q = (uint32_t *)&s->vram[(p << 2) & s->vram_mask];
        *q = (*q & ~wm) | (v & wm);
    }
}

uint16_t s3v_mem_read16(S3VState *s, uint32_t addr)
{
    if (enhanced(s) && !(s->accel && s3a_mmio_hit_legacy(s->accel, addr))) {
        int32_t off = win_off(s, addr);
        if (off >= 0 && (off & 0xffff) != 0xffff) {
            uint32_t i = ((s->bank << 16) + off) & s->vram_mask;
            if (i + 1 < s->vram_size)
                return *(uint16_t *)&s->vram[i];
        }
    }
    return s3v_mem_read8(s, addr) | (s3v_mem_read8(s, addr + 1) << 8);
}

void s3v_mem_write16(S3VState *s, uint32_t addr, uint16_t val)
{
    if (enhanced(s) && !(s->accel && s3a_mmio_hit_legacy(s->accel, addr))) {
        int32_t off = win_off(s, addr);
        if (off >= 0 && (off & 0xffff) != 0xffff) {
            uint32_t i = ((s->bank << 16) + off) & s->vram_mask;
            if (i + 1 < s->vram_size) {
                *(uint16_t *)&s->vram[i] = val;
                return;
            }
        }
    }
    s3v_mem_write8(s, addr, val);
    s3v_mem_write8(s, addr + 1, val >> 8);
}

uint32_t s3v_mem_read32(S3VState *s, uint32_t addr)
{
    if (s->accel && s3a_mmio_hit_legacy(s->accel, addr))
        return s3a_mmio_read32(s->accel, addr);
    if (enhanced(s)) {
        int32_t off = win_off(s, addr);
        if (off >= 0 && (off & 0xffff) <= 0xfffc) {
            uint32_t i = ((s->bank << 16) + off) & s->vram_mask;
            if (i + 3 < s->vram_size)
                return *(uint32_t *)&s->vram[i];
        }
    }
    return s3v_mem_read16(s, addr) | ((uint32_t)s3v_mem_read16(s, addr + 2) << 16);
}

void s3v_mem_write32(S3VState *s, uint32_t addr, uint32_t val)
{
    if (s->accel && s3a_mmio_hit_legacy(s->accel, addr)) {
        s3a_mmio_write32(s->accel, addr, val);
        return;
    }
    if (enhanced(s)) {
        int32_t off = win_off(s, addr);
        if (off >= 0 && (off & 0xffff) <= 0xfffc) {
            uint32_t i = ((s->bank << 16) + off) & s->vram_mask;
            if (i + 3 < s->vram_size) {
                *(uint32_t *)&s->vram[i] = val;
                return;
            }
        }
    }
    s3v_mem_write16(s, addr, val);
    s3v_mem_write16(s, addr + 2, val >> 16);
}

/* ---- linear aperture ---- */

static inline uint32_t law_base(S3VState *s)
{
    return ((uint32_t)s->cr[0x59] << 24) | ((uint32_t)s->cr[0x5a] << 16);
}

bool s3v_lfb_hit(S3VState *s, uint32_t addr)
{
    if (!(s->cr[0x58] & 0x10))
        return false;
    uint32_t base = law_base(s) & 0xfc000000u;
    return (addr - base) < 0x04000000u &&
        ((addr - base) < 0x01000000u ||
         (s->accel && (addr - base) < 0x01010000u));
}

/* offset inside the 64MB window: 0-16MB frame buffer (aliased), 16MB+ MMIO */
static inline uint32_t lfb_off(S3VState *s, uint32_t addr)
{
    return addr - (law_base(s) & 0xfc000000u);
}

uint8_t s3v_lfb_read8(S3VState *s, uint32_t addr)
{
    uint32_t o = lfb_off(s, addr);
    if (o >= 0x01000000u)
        return s3a_mmio_read8(s->accel, o);
    return s->vram[o & s->vram_mask];
}

void s3v_lfb_write8(S3VState *s, uint32_t addr, uint8_t val)
{
    uint32_t o = lfb_off(s, addr);
    if (o >= 0x01000000u) {
        s3a_mmio_write8(s->accel, o, val);
        return;
    }
    s->vram[o & s->vram_mask] = val;
}

uint16_t s3v_lfb_read16(S3VState *s, uint32_t addr)
{
    uint32_t o = lfb_off(s, addr);
    if (o >= 0x01000000u)
        return s3a_mmio_read16(s->accel, o);
    o &= s->vram_mask;
    if (o + 1 < s->vram_size)
        return *(uint16_t *)&s->vram[o];
    return s->vram[o] | (s->vram[0] << 8);
}

void s3v_lfb_write16(S3VState *s, uint32_t addr, uint16_t val)
{
    uint32_t o = lfb_off(s, addr);
    if (o >= 0x01000000u) {
        s3a_mmio_write16(s->accel, o, val);
        return;
    }
    o &= s->vram_mask;
    if (o + 1 < s->vram_size)
        *(uint16_t *)&s->vram[o] = val;
}

uint32_t s3v_lfb_read32(S3VState *s, uint32_t addr)
{
    uint32_t o = lfb_off(s, addr);
    if (o >= 0x01000000u)
        return s3a_mmio_read32(s->accel, o);
    o &= s->vram_mask;
    if (o + 3 < s->vram_size)
        return *(uint32_t *)&s->vram[o];
    return 0;
}

void s3v_lfb_write32(S3VState *s, uint32_t addr, uint32_t val)
{
    uint32_t o = lfb_off(s, addr);
    if (o >= 0x01000000u) {
        s3a_mmio_write32(s->accel, o, val);
        return;
    }
    o &= s->vram_mask;
    if (o + 3 < s->vram_size)
        *(uint32_t *)&s->vram[o] = val;
}

/* ------------------------------------------------------------------ */
/* rendering                                                          */

static inline uint32_t start_address(S3VState *s)
{
    uint32_t a = s->cr[0x0d] | (s->cr[0x0c] << 8);
    uint32_t ext;
    if (s->cr[0x69] & 0x1f)
        ext = s->cr[0x69] & 0x1f;
    else
        ext = ((s->cr[0x31] >> 4) & 3) | ((s->cr[0x51] & 3) << 2);
    return a | (ext << 16);
}

static inline uint32_t row_offset(S3VState *s)
{
    uint32_t o = s->cr[0x13];
    if (s->cr[0x51] & 0x30)
        o |= (s->cr[0x51] & 0x30) << 4;
    else if (s->cr[0x43] & 0x04)
        o |= 0x100;
    return o;
}

static inline uint32_t hdisp_chars(S3VState *s)
{
    return s->cr[1] + 1 + ((s->cr[0x5d] & 2) ? 256 : 0);
}

static inline uint32_t line_compare(S3VState *s)
{
    return s->cr[0x18] | ((s->cr[7] & 0x10) << 4) | ((s->cr[9] & 0x40) << 3) |
        ((s->cr[0x5e] & 0x40) << 4);
}

/* VGA memory address generation: ma (char clock counter) -> planar offset */
static inline uint32_t vga_addr(S3VState *s, uint32_t ma, int rowscan)
{
    uint32_t a;
    if (s->cr[0x14] & 0x40)
        a = ma << 2;
    else if (!(s->cr[0x17] & 0x40))
        a = ((ma << 1) & 0x1fffe) | ((ma >> ((s->cr[0x17] & 0x20) ? 15 : 13)) & 1) |
            (ma & ~0xffffu) << 1;
    else
        a = ma;
    if (!(s->cr[0x17] & 1))
        a = (a & ~(1u << 13)) | ((rowscan & 1) << 13);
    if (!(s->cr[0x17] & 2))
        a = (a & ~(1u << 14)) | (((rowscan >> 1) & 1) << 14);
    return a;
}

static inline uint32_t ar_color(S3VState *s, int v)
{
    v &= s->ar[0x12] & 0x0f;
    uint32_t c = s->ar[v];
    if (s->ar[0x10] & 0x80)
        c = ((s->ar[0x14] & 0x0f) << 4) | (c & 0x0f);
    else
        c = ((s->ar[0x14] & 0x0c) << 4) | (c & 0x3f);
    return s->pal32[c & s->dac_mask];
}

static void render_text(S3VState *s, uint32_t *fb, int pitch, int *pw, int *ph)
{
    int cw = (s->sr[1] & 1) ? 8 : 9;
    int dclk2 = (s->sr[1] & 0x08) ? 2 : 1;
    int cols = hdisp_chars(s);
    int maxscan = (s->cr[9] & 0x1f) + 1;
    int dbl = (s->cr[9] & 0x80) ? 2 : 1;
    int lines = vdisp_lines(s) / dbl;
    int w = cols * cw * dclk2;
    if (cols > 200 || w > S3V_MAX_W) { cols = 80; w = cols * cw * dclk2; }
    if (lines > S3V_MAX_H) lines = S3V_MAX_H;
    if (lines < 1) lines = 1;

    uint32_t ma_row = start_address(s);
    uint32_t off = row_offset(s) * 2;
    uint32_t lc = line_compare(s);
    int rowscan = s->cr[8] & 0x1f;
    uint32_t cursor = s->cr[0x0f] | (s->cr[0x0e] << 8);
    int cs = s->cr[0x0a] & 0x1f, ce = s->cr[0x0b] & 0x1f;
    bool cur_on = !(s->cr[0x0a] & 0x20);
    uint32_t t = get_uticks();
    bool blink_cur = (t / 133333) & 1;   /* ~ 8 frames */
    bool blink_chr = (t / 266666) & 1;   /* ~ 16 frames */
    bool blink_en = (s->ar[0x10] & 0x08) != 0;
    bool lg = (s->ar[0x10] & 0x04) != 0;
    bool mono = (s->ar[0x10] & 0x02) != 0;
    int pan = s->ar[0x13] & 0x0f;
    if (pan >= 8 && cw == 8) pan = 0;
    (void)pan;

    uint8_t cmap = s->sr[3];
    static const uint32_t mapoff[8] = { 0x0000, 0x4000, 0x8000, 0xc000,
                                        0x2000, 0x6000, 0xa000, 0xe000 };
    uint32_t fontA = mapoff[((cmap >> 5) & 1) << 2 | ((cmap >> 2) & 3)];
    uint32_t fontB = mapoff[((cmap >> 4) & 1) << 2 | (cmap & 3)];
    uint32_t pmask = (s->vram_mask >> 2);

    for (int y = 0; y < lines; y++) {
        if ((uint32_t)y == lc) {
            ma_row = 0;
            rowscan = 0;
        }
        uint32_t *d = fb + y * pitch;
        for (int c = 0; c < cols; c++) {
            uint32_t ma = ma_row + c;
            uint32_t a = vga_addr(s, ma, rowscan) & pmask;
            uint8_t ch = s->vram[a << 2];
            uint8_t at = s->vram[(a << 2) | 1];
            uint32_t fo = ((at & 8) ? fontA : fontB) + ch * 32 + rowscan;
            if (s->sr[4] & 0x02)
                ; /* extended memory: 256K; map selection already applied */
            uint8_t bits = s->vram[((fo & pmask) << 2) | 2];
            int fg = at & 0x0f, bg = at >> 4;
            if (blink_en) {
                bg &= 7;
                if ((at & 0x80) && blink_chr)
                    bits = 0;
            }
            if (mono && (at & 0x77) == 0x01 &&
                rowscan == (s->cr[0x14] & 0x1f))
                bits = 0xff;
            if (cur_on && ma == cursor && rowscan >= cs && rowscan <= ce &&
                blink_cur)
                bits = 0xff;
            uint32_t fgc = ar_color(s, fg), bgc = ar_color(s, bg);
            int ninth = (cw == 9) ? ((lg && ch >= 0xc0 && ch <= 0xdf) ? (bits & 1) : 0) : 0;
            uint32_t *q = d + c * cw * dclk2;
            for (int i = 0; i < 8; i++) {
                uint32_t col = (bits & (0x80 >> i)) ? fgc : bgc;
                *q++ = col;
                if (dclk2 == 2) *q++ = col;
            }
            if (cw == 9) {
                uint32_t col = ninth ? fgc : bgc;
                *q++ = col;
                if (dclk2 == 2) *q++ = col;
            }
        }
        if (++rowscan >= maxscan) {
            rowscan = 0;
            ma_row += off;
        }
    }
    *pw = w;
    *ph = lines;
}

/* standard VGA graphics: 16 colour planar, CGA 4 colour, 256 colour */
static void render_vga_gfx(S3VState *s, uint32_t *fb, int pitch, int *pw, int *ph)
{
    int cols = hdisp_chars(s);
    int maxscan = (s->cr[9] & 0x1f) + 1;
    int dbl = (s->cr[9] & 0x80) ? 2 : 1;
    int sl_total = vdisp_lines(s) / dbl;
    bool rows_only = (s->cr[0x17] & 3) == 3 && maxscan > 1;
    int lines = rows_only ? (sl_total + maxscan - 1) / maxscan : sl_total;
    int mode256 = (s->gr[5] & 0x40) != 0;
    int cga = (s->gr[5] & 0x20) != 0;
    int w = mode256 ? cols * 4 : cols * 8;
    if (w > S3V_MAX_W) w = S3V_MAX_W;
    if (lines > S3V_MAX_H) lines = S3V_MAX_H;
    if (lines < 1) lines = 1;

    uint32_t ma_row = start_address(s);
    uint32_t off = row_offset(s) * 2;
    uint32_t lc = line_compare(s);
    int rowscan = s->cr[8] & 0x1f;
    int pan = s->ar[0x13] & 0x0f;
    uint32_t pmask = (s->vram_mask >> 2);
    if (!enhanced(s) && (s->cr[0x32] & 0x40))
        pmask = 0xffff;
    bool lin = enhanced(s);

    int y = 0;
    for (int sl = 0; sl < sl_total && y < lines; sl++) {
        if ((uint32_t)sl == lc) {
            ma_row = 0;
            rowscan = 0;
            if (s->ar[0x10] & 0x20)
                pan = 0;
        }
        bool draw = !rows_only || rowscan == 0;
        if (draw) {
            uint32_t *d = fb + y * pitch;
            if (mode256) {
                if (lin) {
                    uint32_t base = ma_row * 4;
                    for (int x = 0; x < w; x++)
                        d[x] = s->pal32[s->vram[(base + x) & s->vram_mask] & s->dac_mask];
                } else {
                    int skip = (pan >> 1) & 3;
                    int x = 0;
                    for (int c = 0; x < w; c++) {
                        uint32_t a = vga_addr(s, ma_row + c, rowscan) & pmask;
                        const uint8_t *p = &s->vram[a << 2];
                        for (int k = 0; k < 4 && x < w; k++) {
                            if (skip) { skip--; continue; }
                            d[x++] = s->pal32[p[k] & s->dac_mask];
                        }
                    }
                }
            } else {
                int skip = pan & 7;
                int x = 0;
                for (int c = 0; x < w; c++) {
                    uint32_t a = vga_addr(s, ma_row + c, rowscan) & pmask;
                    uint32_t pl = *(uint32_t *)&s->vram[a << 2];
                    uint8_t b0 = pl, b1 = pl >> 8, b2 = pl >> 16, b3 = pl >> 24;
                    for (int k = 0; k < 8 && x < w; k++) {
                        int v;
                        if (cga) {
                            uint8_t src = (k < 4) ? b0 : b1;
                            int sh = 6 - (k & 3) * 2;
                            v = (src >> sh) & 3;
                            uint8_t src2 = (k < 4) ? b2 : b3;
                            v |= ((src2 >> sh) & 3) << 2;
                        } else {
                            int m = 7 - k;
                            v = ((b0 >> m) & 1) | (((b1 >> m) & 1) << 1) |
                                (((b2 >> m) & 1) << 2) | (((b3 >> m) & 1) << 3);
                        }
                        if (skip) { skip--; continue; }
                        d[x++] = ar_color(s, v);
                    }
                }
            }
            y++;
        }
        if (dbl == 1 || 1) {
            if (++rowscan >= maxscan) {
                rowscan = 0;
                ma_row += off;
            }
        }
    }
    *pw = w;
    *ph = y;
}

static int s3_bpp(S3VState *s)
{
    switch (s->cr[0x67] >> 4) {
    case 2: case 3: return 15;
    case 4: case 5: return 16;
    case 7: return 24;
    case 13: return 32;
    default: return 8;
    }
}

static void draw_hw_cursor(S3VState *s, uint32_t *fb, int pitch, int w, int h, int bpp)
{
    if (!(s->cr[0x45] & 1))
        return;
    int cx = ((s->cr[0x46] & 7) << 8) | s->cr[0x47];
    int cy = ((s->cr[0x48] & 7) << 8) | s->cr[0x49];
    int xo = s->cr[0x4e] & 0x3f, yo = s->cr[0x4f] & 0x3f;
    uint32_t addr = (((s->cr[0x4c] & 0x0f) << 8) | s->cr[0x4d]) * 1024;
    uint32_t fg, bg;
    if (bpp == 8) {
        fg = s->pal32[s->hwc_fg[0]];
        bg = s->pal32[s->hwc_bg[0]];
    } else if (bpp == 15 || bpp == 16) {
        uint16_t f = s->hwc_fg[0] | (s->hwc_fg[1] << 8);
        uint16_t b = s->hwc_bg[0] | (s->hwc_bg[1] << 8);
        if (bpp == 16) {
            fg = ((f >> 11) << 19) | (((f >> 5) & 0x3f) << 10) | ((f & 0x1f) << 3);
            bg = ((b >> 11) << 19) | (((b >> 5) & 0x3f) << 10) | ((b & 0x1f) << 3);
        } else {
            fg = (((f >> 10) & 0x1f) << 19) | (((f >> 5) & 0x1f) << 11) | ((f & 0x1f) << 3);
            bg = (((b >> 10) & 0x1f) << 19) | (((b >> 5) & 0x1f) << 11) | ((b & 0x1f) << 3);
        }
    } else {
        fg = s->hwc_fg[0] | (s->hwc_fg[1] << 8) | (s->hwc_fg[2] << 16);
        bg = s->hwc_bg[0] | (s->hwc_bg[1] << 8) | (s->hwc_bg[2] << 16);
    }
    for (int y = yo; y < 64; y++) {
        int py = cy + y - yo;
        if (py < 0 || py >= h) continue;
        const uint8_t *line = &s->vram[(addr + y * 16) & s->vram_mask];
        for (int x = xo; x < 64; x++) {
            int px = cx + x - xo;
            if (px < 0 || px >= w) continue;
            int word = x >> 4, bit = 15 - (x & 15);
            uint16_t andw = line[word * 4] << 8 | line[word * 4 + 1];
            uint16_t xorw = line[word * 4 + 2] << 8 | line[word * 4 + 3];
            int a = (andw >> bit) & 1, xo2 = (xorw >> bit) & 1;
            uint32_t *p = &fb[py * pitch + px];
            if (s->cr[0x55] & 0x10) {
                /* X11 mode */
                if (a) *p = xo2 ? fg : bg;
            } else {
                if (!a) *p = xo2 ? fg : bg;
                else if (xo2) *p ^= 0xffffff;
            }
        }
    }
}

static void render_highres(S3VState *s, uint32_t *fb, int pitch, int *pw, int *ph)
{
    int bpp = s3_bpp(s);
    int w = hdisp_chars(s) * 8;
    if (bpp == 15 || bpp == 16) w /= 2;
    else if (bpp == 24) w /= 3;
    int h = vdisp_lines(s);
    if (s->cr[0x42] & 0x20) h *= 2; /* interlaced */
    if (s->cr[9] & 0x80) h /= 2;
    if (w > S3V_MAX_W) w = S3V_MAX_W;
    if (h > S3V_MAX_H) h = S3V_MAX_H;
    uint32_t base = start_address(s) * 4;
    uint32_t stride = row_offset(s) * 8;
    uint32_t m = s->vram_mask;
    for (int y = 0; y < h; y++) {
        uint32_t a = base + y * stride;
        uint32_t *d = fb + y * pitch;
        switch (bpp) {
        case 8:
            for (int x = 0; x < w; x++)
                d[x] = s->pal32[s->vram[(a + x) & m] & s->dac_mask];
            break;
        case 15:
            for (int x = 0; x < w; x++) {
                uint32_t i = (a + x * 2) & m;
                uint16_t p = s->vram[i] | (s->vram[(i + 1) & m] << 8);
                uint32_t r = (p >> 10) & 0x1f, g = (p >> 5) & 0x1f, b = p & 0x1f;
                d[x] = ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
            }
            break;
        case 16:
            for (int x = 0; x < w; x++) {
                uint32_t i = (a + x * 2) & m;
                uint16_t p = s->vram[i] | (s->vram[(i + 1) & m] << 8);
                uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
                d[x] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            }
            break;
        case 24:
            for (int x = 0; x < w; x++) {
                uint32_t i = a + x * 3;
                d[x] = s->vram[i & m] | (s->vram[(i + 1) & m] << 8) |
                    (s->vram[(i + 2) & m] << 16);
            }
            break;
        case 32:
            for (int x = 0; x < w; x++) {
                uint32_t i = (a + x * 4) & m;
                d[x] = (s->vram[i] | (s->vram[(i + 1) & m] << 8) |
                        (s->vram[(i + 2) & m] << 16));
            }
            break;
        }
    }
    draw_hw_cursor(s, fb, pitch, w, h, bpp);
    *pw = w;
    *ph = h;
}

bool s3v_render(S3VState *s, uint32_t *fb, int *w, int *h, int *pitch)
{
    *pitch = S3V_MAX_W;
    if ((s->sr[1] & 0x20) || !(s->ar_idx & 0x20)) {
        /* screen off or palette access mode (display disabled) */
        *w = 640; *h = 480;
        for (int y = 0; y < 480; y++)
            memset(fb + y * S3V_MAX_W, 0, 640 * 4);
        snprintf(s->desc, sizeof(s->desc), "blank");
        return true;
    }
    if (!(s->gr[6] & 1)) {
        render_text(s, fb, S3V_MAX_W, w, h);
        snprintf(s->desc, sizeof(s->desc), "text %dx%d", *w, *h);
    } else if (enhanced(s) && (s->gr[5] & 0x40) && (s->cr[0x3a] & 0x10)) {
        render_highres(s, fb, S3V_MAX_W, w, h);
        snprintf(s->desc, sizeof(s->desc), "s3 %dx%d %dbpp", *w, *h, s3_bpp(s));
    } else {
        render_vga_gfx(s, fb, S3V_MAX_W, w, h);
        snprintf(s->desc, sizeof(s->desc), "vga gfx %dx%d %s", *w, *h,
                 (s->gr[5] & 0x40) ? "256c" : (s->gr[5] & 0x20) ? "cga" : "planar");
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    return true;
}

/* text screen dump (debug): writes rows of characters into out */
int s3v_text_dump(S3VState *s, char *out, int maxlen)
{
    int cols = hdisp_chars(s);
    int maxscan = (s->cr[9] & 0x1f) + 1;
    int rows = vdisp_lines(s) / maxscan;
    uint32_t ma = start_address(s);
    uint32_t off = row_offset(s) * 2;
    int n = 0;
    if (cols > 132) cols = 132;
    if (rows > 60) rows = 60;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols && n < maxlen - 2; c++) {
            uint32_t a = vga_addr(s, ma + c, 0) & (s->vram_mask >> 2);
            uint8_t ch = s->vram[a << 2];
            out[n++] = (ch >= 32 && ch < 127) ? ch : (ch == 0 ? ' ' : '.');
        }
        while (n > 0 && out[n - 1] == ' ') n--;
        if (n < maxlen - 1) out[n++] = '\n';
        ma += off;
    }
    out[n] = 0;
    return n;
}

const char *s3v_mode_desc(S3VState *s)
{
    return s->desc;
}

void s3v_dump_state(S3VState *s)
{
    fprintf(stderr, "misc=%02x\nSR:", s->misc);
    for (int i = 0; i < 0x20; i++) fprintf(stderr, " %02x", s->sr[i]);
    fprintf(stderr, "\nGR:");
    for (int i = 0; i < 9; i++) fprintf(stderr, " %02x", s->gr[i]);
    fprintf(stderr, "\nAR:");
    for (int i = 0; i < 0x15; i++) fprintf(stderr, " %02x", s->ar[i]);
    for (int j = 0; j < 0x70; j += 16) {
        fprintf(stderr, "\nCR%02x:", j);
        for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", s->cr[j + i]);
    }
    fprintf(stderr, "\nbank=%x\n", s->bank);
}

/* ------------------------------------------------------------------ */

void s3v_reset(S3VState *s)
{
    memset(s->sr, 0, sizeof(s->sr));
    memset(s->cr, 0, sizeof(s->cr));
    memset(s->gr, 0, sizeof(s->gr));
    memset(s->ar, 0, sizeof(s->ar));
    s->misc = 0;
    s->ar_ff = 0;
    s->ar_idx = 0;
    s->dac_mask = 0xff;
    s->bank = 0;
    s->sr[4] = 0x02;
    /* identification and straps */
    s->cr[0x2d] = 0x56;
    s->cr[0x2e] = 0x31;
    s->cr[0x2f] = 0x06;
    s->cr[0x30] = 0xe1;
    s->cr[0x36] = 2 | (1 << 4) | ((s->vram_size >= (4u << 20)) ? 0 : (4 << 5));
    s->cr[0x37] = 1 | (7 << 5);
    s->cr[0x59] = 0xe0; /* linear aperture base as if assigned by PCI BIOS */
    s->cr[0x5a] = 0x00;
    s->cr[0x11] = 0;
    s->sr[0x10] = 0x42; s->sr[0x11] = 0x2e; /* MCLK */
    s->sr[0x12] = 0x55; s->sr[0x13] = 0x10;
    update_timing(s);
    if (s->accel)
        s3a_reset(s->accel);
}

S3VState *s3v_new(uint32_t vram_size)
{
    S3VState *s = calloc(1, sizeof(S3VState));
    s->vram_size = vram_size;
    s->vram_mask = vram_size - 1;
    s->vram = calloc(1, vram_size + 4);
    s->t0 = get_uticks();
    s->accel = s3a_new(s->vram, vram_size, s->cr, s,
                       (uint8_t (*)(void *, uint32_t))s3v_io_read,
                       (void (*)(void *, uint32_t, uint8_t))s3v_io_write);
    s3v_reset(s);
    return s;
}

/* PCI BAR0 programming: the linear window follows it */
void s3v_set_pci_base(S3VState *s, uint32_t base, int enabled)
{
    (void)enabled;
    s->cr[0x59] = base >> 24;
    s->cr[0x5a] = base >> 16;
}
