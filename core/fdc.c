/*
 * pc486 - Intel 82077AA floppy disk controller (ports 3F0-3F7, IRQ 6, DMA 2)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fdc.h"
#include "i8257.h"

uint32_t get_uticks(void);

enum { PH_CMD, PH_EXEC, PH_RESULT };

typedef struct {
    FILE *f;
    int ro;
    int tracks, heads, spt;
    int cyl;
    int changed;
} FDrive;

struct FDC {
    FDrive d[2];
    uint8_t dor, dsr, ccr;
    int phase;
    uint8_t cmd[16];
    int cmd_len, cmd_pos;
    uint8_t res[16];
    int res_len, res_pos;
    int nodma;
    /* pending sense interrupt status */
    int sense_cnt;
    uint8_t sense_st0[4];
    uint8_t sense_pcn[4];
    /* data transfer */
    int xfer_dir; /* 0 read (to memory), 1 write (from memory), 2 format, 3 verify */
    int drv, head, cyl, sect, eot, n, mt;
    int fmt_sc, fmt_fill, fmt_count;
    uint8_t st0, st1, st2;
    uint8_t buf[512];
    int irq, dma_chan;
    void *pic;
    void (*set_irq)(void *pic, int irq, int level);
    I8257State *dma;
    int irq_level;
    int irq_delay;
    uint32_t irq_at;
    uint8_t config, precomp;
    int lock;
};

static void fdc_set_irq(FDC *f, int level)
{
    if (!(f->dor & 0x08))
        level = 0;
    if (level != f->irq_level) {
        f->irq_level = level;
        f->set_irq(f->pic, f->irq, level);
    }
}

static void raise_irq_later(FDC *f)
{
    f->irq_delay = 1;
    f->irq_at = get_uticks() + 150;
}

void fdc_step(FDC *f)
{
    if (f->irq_delay && (int32_t)(get_uticks() - f->irq_at) >= 0) {
        f->irq_delay = 0;
        fdc_set_irq(f, 1);
    }
}

static void geometry_from_size(FDrive *d, long size)
{
    static const struct { long size; int t, h, s; } g[] = {
        { 163840, 40, 1, 8 }, { 184320, 40, 1, 9 }, { 327680, 40, 2, 8 },
        { 368640, 40, 2, 9 }, { 737280, 80, 2, 9 }, { 1228800, 80, 2, 15 },
        { 1474560, 80, 2, 18 }, { 1720320, 80, 2, 21 }, { 1763328, 82, 2, 21 },
        { 2949120, 80, 2, 36 },
    };
    for (unsigned i = 0; i < sizeof(g) / sizeof(g[0]); i++) {
        if (g[i].size == size) {
            d->tracks = g[i].t; d->heads = g[i].h; d->spt = g[i].s;
            return;
        }
    }
    /* unknown: assume 1.44M-like layout */
    d->tracks = 80; d->heads = 2; d->spt = 18;
}

int fdc_insert(FDC *f, int drive, const char *path)
{
    FDrive *d = &f->d[drive & 1];
    fdc_eject(f, drive);
    d->f = fopen(path, "r+b");
    d->ro = 0;
    if (!d->f) {
        d->f = fopen(path, "rb");
        d->ro = 1;
    }
    if (!d->f)
        return -1;
    fseek(d->f, 0, SEEK_END);
    long size = ftell(d->f);
    geometry_from_size(d, size);
    d->changed = 1;
    return 0;
}

void fdc_eject(FDC *f, int drive)
{
    FDrive *d = &f->d[drive & 1];
    if (d->f) {
        fclose(d->f);
        d->f = NULL;
    }
    d->changed = 1;
}

int fdc_has_disk(FDC *f, int drive)
{
    return f->d[drive & 1].f != NULL;
}

static void set_result(FDC *f, const uint8_t *r, int n)
{
    memcpy(f->res, r, n);
    f->res_len = n;
    f->res_pos = 0;
    f->phase = n ? PH_RESULT : PH_CMD;
}

static void reset_ctrl(FDC *f, int from_dor)
{
    f->phase = PH_CMD;
    f->cmd_pos = f->cmd_len = 0;
    f->res_len = f->res_pos = 0;
    fdc_set_irq(f, 0);
    f->irq_delay = 0;
    if (from_dor) {
        f->sense_cnt = 4;
        for (int i = 0; i < 4; i++) {
            f->sense_st0[i] = 0xc0 | i;
            f->sense_pcn[i] = i < 2 ? f->d[i].cyl : 0;
        }
        raise_irq_later(f);
    }
    if (f->dma)
        i8257_dma_release_DREQ(f->dma, f->dma_chan);
}

static long sector_offset(FDrive *d, int cyl, int head, int sect)
{
    return ((long)(cyl * d->heads + head) * d->spt + (sect - 1)) * 512;
}

/* finish a read/write/format/verify command */
static void end_xfer(FDC *f)
{
    uint8_t r[7];
    r[0] = f->st0 | (f->head << 2) | f->drv;
    r[1] = f->st1;
    r[2] = f->st2;
    r[3] = f->cyl;
    r[4] = f->head;
    r[5] = f->sect;
    r[6] = f->n;
    set_result(f, r, 7);
    if (f->dma)
        i8257_dma_release_DREQ(f->dma, f->dma_chan);
    raise_irq_later(f);
}

/* check that the current sector exists on the medium */
static int sector_ok(FDC *f)
{
    FDrive *d = &f->d[f->drv];
    if (!d->f) {
        f->st0 = 0x40;
        f->st1 = 0x01; /* missing address mark */
        return 0;
    }
    if (f->cyl >= d->tracks || f->head >= d->heads || f->sect < 1 ||
        f->sect > d->spt || f->n != 2) {
        f->st0 = 0x40;
        f->st1 = 0x04; /* no data */
        return 0;
    }
    return 1;
}

static int advance_sector(FDC *f)
{
    /* returns 0 at end of track/cylinder */
    if (f->sect >= f->eot) {
        f->sect = 1;
        if (f->mt && f->head == 0) {
            f->head = 1;
            return 1;
        }
        if (f->mt)
            f->head = 0;
        f->cyl++;
        return 0;
    }
    f->sect++;
    return 1;
}

static int dma_handler(void *opaque, int nchan, int pos, int len)
{
    FDC *f = opaque;
    FDrive *d = &f->d[f->drv];
    if (f->phase != PH_EXEC)
        return pos;

    if (f->xfer_dir == 2) {
        /* format: 4 bytes (C, H, R, N) per sector */
        while (pos + 4 <= len && f->fmt_count < f->fmt_sc) {
            uint8_t id[4];
            i8257_dma_read_memory(f->dma, nchan, id, pos, 4);
            pos += 4;
            if (d->f && !d->ro && id[0] < d->tracks && id[1] < d->heads &&
                id[2] >= 1 && id[2] <= d->spt) {
                memset(f->buf, f->fmt_fill, 512);
                fseek(d->f, sector_offset(d, id[0], id[1], id[2]), SEEK_SET);
                fwrite(f->buf, 1, 512, d->f);
            }
            f->fmt_count++;
        }
        if (f->fmt_count >= f->fmt_sc || pos >= len) {
            if (!d->f) { f->st0 = 0x40; f->st1 = 0x01; }
            else if (d->ro) { f->st0 = 0x40; f->st1 = 0x02; }
            fflush(d->f);
            end_xfer(f);
        }
        return pos;
    }

    while (pos < len) {
        if (!sector_ok(f)) {
            end_xfer(f);
            return pos;
        }
        int chunk = 512;
        if (pos + chunk > len)
            chunk = len - pos;
        long off = sector_offset(d, f->cyl, f->head, f->sect);
        if (f->xfer_dir == 0 || f->xfer_dir == 3) {
            fseek(d->f, off, SEEK_SET);
            if (fread(f->buf, 1, 512, d->f) != 512)
                memset(f->buf, 0, 512);
            if (f->xfer_dir == 0)
                i8257_dma_write_memory(f->dma, nchan, f->buf, pos, chunk);
        } else {
            if (d->ro) {
                f->st0 = 0x40;
                f->st1 = 0x02; /* not writable */
                end_xfer(f);
                return pos;
            }
            i8257_dma_read_memory(f->dma, nchan, f->buf, pos, chunk);
            if (chunk < 512)
                memset(f->buf + chunk, 0, 512 - chunk);
            fseek(d->f, off, SEEK_SET);
            fwrite(f->buf, 1, 512, d->f);
            fflush(d->f);
        }
        pos += chunk;
        int more = advance_sector(f);
        if (!more) {
            end_xfer(f);
            return pos;
        }
    }
    /* terminal count reached */
    end_xfer(f);
    return pos;
}

static int cmd_length(uint8_t c)
{
    switch (c & 0x1f) {
    case 0x02: return 9; /* read track */
    case 0x03: return 3; /* specify */
    case 0x04: return 2; /* sense drive status */
    case 0x05: return 9; /* write data */
    case 0x06: return 9; /* read data */
    case 0x07: return 2; /* recalibrate */
    case 0x08: return 1; /* sense interrupt */
    case 0x09: return 9; /* write deleted */
    case 0x0a: return 2; /* read id */
    case 0x0c: return 9; /* read deleted */
    case 0x0d: return 6; /* format track */
    case 0x0e: return 1; /* dumpreg */
    case 0x0f: return 3; /* seek */
    case 0x10: return 1; /* version */
    case 0x11: return 9; /* scan equal */
    case 0x12: return 2; /* perpendicular */
    case 0x13: return 4; /* configure */
    case 0x14: return 1; /* lock */
    case 0x16: return 9; /* verify */
    case 0x18: return 1; /* part id */
    case 0x19: return 9; /* scan low or equal */
    case 0x1d: return 9; /* scan high or equal */
    }
    return 1;
}

static void start_xfer(FDC *f, int dir)
{
    uint8_t *c = f->cmd;
    f->mt = (c[0] & 0x80) != 0;
    f->drv = c[1] & 1;
    f->head = (c[1] >> 2) & 1;
    f->cyl = c[2];
    f->head = c[3] & 1;
    f->sect = c[4];
    f->n = c[5];
    f->eot = c[6];
    f->st0 = f->st1 = f->st2 = 0;
    f->xfer_dir = dir;
    FDrive *d = &f->d[f->drv];
    d->cyl = f->cyl;
    if (!d->f) {
        /* no medium: the real controller would wait forever for an index
         * pulse; report "not ready" so the BIOS times out quickly */
        f->st0 = 0x48;
        end_xfer(f);
        return;
    }
    if (!sector_ok(f)) {
        end_xfer(f);
        return;
    }
    f->phase = PH_EXEC;
    if (dir == 3) {
        /* verify: no DMA */
        do {
            if (!sector_ok(f))
                break;
        } while (advance_sector(f));
        end_xfer(f);
        return;
    }
    if (f->nodma) {
        /* non-DMA mode is not supported */
        f->st0 = 0x40;
        f->st1 = 0x10;
        end_xfer(f);
        return;
    }
    i8257_dma_hold_DREQ(f->dma, f->dma_chan);
}

static void exec_cmd(FDC *f)
{
    uint8_t *c = f->cmd;
    uint8_t r[16];
    int drv = c[1] & 1;
    switch (c[0] & 0x1f) {
    case 0x03: /* specify */
        f->nodma = c[2] & 1;
        set_result(f, r, 0);
        break;
    case 0x04: { /* sense drive status */
        FDrive *d = &f->d[drv];
        r[0] = (c[1] & 7) | 0x20 | (d->cyl == 0 ? 0x10 : 0) |
               (d->heads == 2 || !d->f ? 0x08 : 0) | ((d->f && d->ro) ? 0x40 : 0);
        set_result(f, r, 1);
        break;
    }
    case 0x07: /* recalibrate */
        f->d[drv].cyl = 0;
        if (f->d[drv].f) f->d[drv].changed = 0;
        f->sense_cnt = 1;
        f->sense_st0[0] = 0x20 | drv;
        f->sense_pcn[0] = 0;
        set_result(f, r, 0);
        raise_irq_later(f);
        break;
    case 0x0f: /* seek */
        f->d[drv].cyl = c[2];
        if (f->d[drv].f) f->d[drv].changed = 0;
        f->sense_cnt = 1;
        f->sense_st0[0] = 0x20 | (c[1] & 7);
        f->sense_pcn[0] = c[2];
        set_result(f, r, 0);
        raise_irq_later(f);
        break;
    case 0x08: /* sense interrupt status */
        fdc_set_irq(f, 0);
        if (f->sense_cnt > 0) {
            int i = 0;
            /* return entries in order */
            r[0] = f->sense_st0[i];
            r[1] = f->sense_pcn[i];
            for (int k = 1; k < f->sense_cnt; k++) {
                f->sense_st0[k - 1] = f->sense_st0[k];
                f->sense_pcn[k - 1] = f->sense_pcn[k];
            }
            f->sense_cnt--;
            set_result(f, r, 2);
        } else {
            r[0] = 0x80;
            set_result(f, r, 1);
        }
        break;
    case 0x0a: { /* read id */
        FDrive *d = &f->d[drv];
        f->drv = drv;
        f->head = (c[1] >> 2) & 1;
        f->cyl = d->cyl;
        f->sect = 1;
        f->n = 2;
        f->st0 = f->st1 = f->st2 = 0;
        if (!d->f) {
            f->st0 = 0x40;
            f->st1 = 0x01;
        }
        end_xfer(f);
        break;
    }
    case 0x06: case 0x0c: case 0x02:
        start_xfer(f, 0);
        break;
    case 0x05: case 0x09:
        start_xfer(f, 1);
        break;
    case 0x16:
        start_xfer(f, 3);
        break;
    case 0x0d: { /* format track */
        FDrive *d = &f->d[drv];
        f->drv = drv;
        f->head = (c[1] >> 2) & 1;
        f->cyl = d->cyl;
        f->n = c[2];
        f->fmt_sc = c[3];
        f->fmt_fill = c[5];
        f->fmt_count = 0;
        f->sect = 1;
        f->st0 = f->st1 = f->st2 = 0;
        f->xfer_dir = 2;
        if (!d->f || d->ro) {
            if (!d->f) { f->st0 = 0x48; } else { f->st0 = 0x40; f->st1 = 0x02; }
            end_xfer(f);
            break;
        }
        f->phase = PH_EXEC;
        i8257_dma_hold_DREQ(f->dma, f->dma_chan);
        break;
    }
    case 0x0e: /* dumpreg */
        r[0] = f->d[0].cyl; r[1] = f->d[1].cyl; r[2] = 0; r[3] = 0;
        r[4] = 0; r[5] = f->nodma; r[6] = 18;
        r[7] = f->lock << 7; r[8] = f->config; r[9] = f->precomp;
        set_result(f, r, 10);
        break;
    case 0x10: /* version */
        r[0] = 0x90;
        set_result(f, r, 1);
        break;
    case 0x12: /* perpendicular mode */
        set_result(f, r, 0);
        break;
    case 0x13: /* configure */
        f->config = c[2];
        f->precomp = c[3];
        set_result(f, r, 0);
        break;
    case 0x14: /* lock */
        f->lock = (c[0] >> 7) & 1;
        r[0] = f->lock << 4;
        set_result(f, r, 1);
        break;
    case 0x18: /* part id */
        r[0] = 0x41;
        set_result(f, r, 1);
        break;
    default:
        r[0] = 0x80;
        set_result(f, r, 1);
        break;
    }
}

uint8_t fdc_ioport_read(FDC *f, int port)
{
    switch (port & 7) {
    case 2: /* DOR is write only on 82077 PS/2 mode, readable on AT mode */
        return f->dor;
    case 3:
        return 0x00; /* tape drive register */
    case 4: { /* MSR */
        uint8_t v = 0;
        if (f->phase == PH_CMD) {
            v = 0x80;
            if (f->cmd_pos) v |= 0x10;
        } else if (f->phase == PH_RESULT) {
            v = 0xc0 | 0x10;
        } else {
            v = 0x10 | (f->nodma ? 0x20 : 0);
        }
        return v;
    }
    case 5: /* data */
        if (f->phase == PH_RESULT) {
            uint8_t v = f->res[f->res_pos++];
            if (f->res_pos >= f->res_len) {
                f->phase = PH_CMD;
                f->res_len = f->res_pos = 0;
            }
            if (f->res_pos == 1)
                fdc_set_irq(f, 0);
            return v;
        }
        return 0xff;
    case 7: { /* DIR */
        FDrive *d = &f->d[f->dor & 1];
        return d->changed ? 0x80 : 0x00;
    }
    }
    return 0xff;
}

void fdc_ioport_write(FDC *f, int port, uint8_t val)
{
    switch (port & 7) {
    case 2: { /* DOR */
        uint8_t old = f->dor;
        f->dor = val;
        if (!(val & 0x04)) {
            reset_ctrl(f, 0);
        } else if (!(old & 0x04)) {
            reset_ctrl(f, 1);
        }
        if (!(val & 0x08))
            fdc_set_irq(f, 0);
        break;
    }
    case 4: /* DSR */
        f->dsr = val;
        if (val & 0x80)
            reset_ctrl(f, 1);
        break;
    case 5: /* data */
        if (f->phase == PH_RESULT) {
            /* writing during result phase: abort result */
            f->phase = PH_CMD;
            f->cmd_pos = 0;
        }
        if (f->phase != PH_CMD)
            break;
        if (f->cmd_pos == 0) {
            f->cmd_len = cmd_length(val);
            fdc_set_irq(f, 0);
        }
        f->cmd[f->cmd_pos++] = val;
        if (f->cmd_pos >= f->cmd_len) {
            f->cmd_pos = 0;
            exec_cmd(f);
        }
        break;
    case 7: /* CCR */
        f->ccr = val;
        break;
    }
}

FDC *fdc_new(int irq, int dma_chan, void *pic,
             void (*set_irq)(void *pic, int irq, int level), void *dma)
{
    FDC *f = calloc(1, sizeof(FDC));
    f->irq = irq;
    f->dma_chan = dma_chan;
    f->pic = pic;
    f->set_irq = set_irq;
    f->dma = dma;
    for (int i = 0; i < 2; i++) {
        f->d[i].tracks = 80; f->d[i].heads = 2; f->d[i].spt = 18;
        f->d[i].changed = 1;
    }
    i8257_dma_register_channel(dma, dma_chan, dma_handler, f);
    f->dor = 0x0c;
    return f;
}
