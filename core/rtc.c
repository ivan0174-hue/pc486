/*
 * pc486 - MC146818 RTC / CMOS RAM (128 bytes at 70h/71h, extra 128 at 72h/73h)
 */
#include <stdlib.h>
#include <string.h>
#include "rtc.h"

uint32_t get_uticks(void);

#define REG_A 10
#define REG_B 11
#define REG_C 12
#define REG_D 13

struct RTC {
    uint8_t ram[256];
    uint8_t idx, idx2;
    int irq;
    void *pic;
    void (*set_irq)(void *pic, int irq, int level);
    uint32_t last_us;       /* time of last second update */
    uint32_t sub_us;        /* microseconds into the current second */
    uint32_t per_last;
    int dirty;
};

static int to_bin(RTC *r, int v)
{
    if (r->ram[REG_B] & 0x04)
        return v;
    return ((v >> 4) * 10) + (v & 0x0f);
}

static int from_bin(RTC *r, int v)
{
    if (r->ram[REG_B] & 0x04)
        return v;
    return ((v / 10) << 4) | (v % 10);
}

static int get_hour(RTC *r)
{
    int h = r->ram[4];
    if (!(r->ram[REG_B] & 0x02)) {
        int pm = h & 0x80;
        h = to_bin(r, h & 0x7f) % 12;
        if (pm) h += 12;
        return h;
    }
    return to_bin(r, h);
}

static void set_hour(RTC *r, int h)
{
    if (!(r->ram[REG_B] & 0x02)) {
        int pm = h >= 12;
        int hh = h % 12;
        if (hh == 0) hh = 12;
        r->ram[4] = from_bin(r, hh) | (pm ? 0x80 : 0);
    } else {
        r->ram[4] = from_bin(r, h);
    }
}

static int days_in_month(int m, int y)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return d[(m - 1) % 12];
}

static void raise(RTC *r, uint8_t flag)
{
    r->ram[REG_C] |= flag;
    if (r->ram[REG_C] & r->ram[REG_B] & 0x70) {
        if (!(r->ram[REG_C] & 0x80)) {
            r->ram[REG_C] |= 0x80;
            r->set_irq(r->pic, r->irq, 1);
        }
    }
}

static void tick_second(RTC *r)
{
    if (r->ram[REG_B] & 0x80)
        return; /* SET: updates inhibited */
    int sec = to_bin(r, r->ram[0]) + 1;
    int min = to_bin(r, r->ram[2]);
    int hour = get_hour(r);
    int dow = to_bin(r, r->ram[6]);
    int day = to_bin(r, r->ram[7]);
    int mon = to_bin(r, r->ram[8]);
    int year = to_bin(r, r->ram[9]);
    int cent = to_bin(r, r->ram[0x32]);
    if (sec >= 60) {
        sec = 0;
        if (++min >= 60) {
            min = 0;
            if (++hour >= 24) {
                hour = 0;
                dow = dow % 7 + 1;
                if (mon < 1 || mon > 12) mon = 1;
                if (++day > days_in_month(mon, cent * 100 + year)) {
                    day = 1;
                    if (++mon > 12) {
                        mon = 1;
                        if (++year >= 100) {
                            year = 0;
                            cent++;
                            r->ram[0x32] = from_bin(r, cent);
                        }
                    }
                }
            }
        }
    }
    r->ram[0] = from_bin(r, sec);
    r->ram[2] = from_bin(r, min);
    set_hour(r, hour);
    r->ram[6] = from_bin(r, dow);
    r->ram[7] = from_bin(r, day);
    r->ram[8] = from_bin(r, mon);
    r->ram[9] = from_bin(r, year);

    raise(r, 0x10); /* update ended */
    /* alarm: don't care values have bits 7:6 set */
    if (((r->ram[1] & 0xc0) == 0xc0 || r->ram[1] == r->ram[0]) &&
        ((r->ram[3] & 0xc0) == 0xc0 || r->ram[3] == r->ram[2]) &&
        ((r->ram[5] & 0xc0) == 0xc0 || r->ram[5] == r->ram[4]))
        raise(r, 0x20);
}

void rtc_step(RTC *r)
{
    uint32_t now = get_uticks();
    uint32_t d = now - r->last_us;
    /* divider chain must be in normal operation (010) */
    if (((r->ram[REG_A] >> 4) & 7) != 2) {
        r->last_us = now;
        return;
    }
    if (d >= 1000000) {
        int n = d / 1000000;
        if (n > 5) n = 5; /* don't race after long pauses */
        for (int i = 0; i < n; i++)
            tick_second(r);
        r->last_us += (d / 1000000) * 1000000;
    }
    /* periodic interrupt */
    int rs = r->ram[REG_A] & 0x0f;
    if (rs && (r->ram[REG_B] & 0x40)) {
        if (rs <= 2) rs += 7;
        uint32_t period = (uint32_t)(1000000ull * (1u << (rs - 1)) / 32768u);
        if (period < 100) period = 100; /* limit to 10 kHz */
        if (now - r->per_last >= period) {
            uint32_t k = (now - r->per_last) / period;
            r->per_last += k * period;
            if (now - r->per_last > 100000)
                r->per_last = now;
            raise(r, 0x40);
        }
    } else {
        r->per_last = now;
    }
}

static uint8_t read_reg(RTC *r, int i)
{
    uint8_t v;
    switch (i) {
    case REG_A: {
        uint32_t d = get_uticks() - r->last_us;
        v = r->ram[REG_A] & 0x7f;
        /* UIP is set during the last 244us before an update */
        if (!(r->ram[REG_B] & 0x80) && d >= 1000000 - 2000)
            v |= 0x80;
        return v;
    }
    case REG_C:
        v = r->ram[REG_C];
        r->ram[REG_C] = 0;
        if (v & 0x80)
            r->set_irq(r->pic, r->irq, 0);
        return v;
    case REG_D:
        return 0x80;
    }
    return r->ram[i];
}

static void write_reg(RTC *r, int i, uint8_t val)
{
    switch (i) {
    case REG_A:
        r->ram[REG_A] = val & 0x7f;
        break;
    case REG_B: {
        uint8_t old = r->ram[REG_B];
        if (val & 0x80)
            val &= ~0x10;
        r->ram[REG_B] = val;
        if (!(val & 0x80) && (old & 0x80))
            r->last_us = get_uticks();
        if ((r->ram[REG_C] & 0x80) && !(r->ram[REG_C] & val & 0x70)) {
            r->ram[REG_C] &= ~0x80;
            r->set_irq(r->pic, r->irq, 0);
        }
        break;
    }
    case REG_C: case REG_D:
        break;
    default:
        if (r->ram[i] != val) {
            r->ram[i] = val;
            if (i >= 0x0e)
                r->dirty = 1;
        }
        break;
    }
}

uint8_t rtc_ioport_read(RTC *r, int port)
{
    switch (port) {
    case 0x71: return read_reg(r, r->idx & 0x7f);
    case 0x73: return r->ram[0x80 | (r->idx2 & 0x7f)];
    }
    return 0xff;
}

void rtc_ioport_write(RTC *r, int port, uint8_t val)
{
    switch (port) {
    case 0x70: r->idx = val & 0x7f; break; /* bit 7: NMI mask */
    case 0x71: write_reg(r, r->idx & 0x7f, val); break;
    case 0x72: r->idx2 = val; break;
    case 0x73:
        if (r->ram[0x80 | (r->idx2 & 0x7f)] != val) {
            r->ram[0x80 | (r->idx2 & 0x7f)] = val;
            r->dirty = 1;
        }
        break;
    }
}

void rtc_set_time(RTC *r, int year, int mon, int day, int wday,
                  int hour, int min, int sec)
{
    r->ram[0] = from_bin(r, sec);
    r->ram[2] = from_bin(r, min);
    set_hour(r, hour);
    r->ram[6] = from_bin(r, wday + 1);
    r->ram[7] = from_bin(r, day);
    r->ram[8] = from_bin(r, mon);
    r->ram[9] = from_bin(r, year % 100);
    r->ram[0x32] = from_bin(r, year / 100);
    r->last_us = get_uticks();
}

uint8_t *rtc_nvram(RTC *r)
{
    return r->ram;
}

int rtc_take_dirty(RTC *r)
{
    int d = r->dirty;
    r->dirty = 0;
    return d;
}

void rtc_load_nvram(RTC *r, const uint8_t *data, int len)
{
    if (len > 256) len = 256;
    /* keep clock registers sane, take everything else */
    for (int i = 0; i < len; i++) {
        if (i == REG_C || i == REG_D)
            continue;
        r->ram[i] = data[i];
    }
    r->ram[REG_A] = (r->ram[REG_A] & 0x0f) | 0x20;
    r->ram[REG_B] &= ~0x80;
}

RTC *rtc_new(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level))
{
    RTC *r = calloc(1, sizeof(RTC));
    r->irq = irq;
    r->pic = pic;
    r->set_irq = set_irq;
    r->ram[REG_A] = 0x26;
    r->ram[REG_B] = 0x02;
    r->ram[REG_D] = 0x80;
    r->last_us = r->per_last = get_uticks();
    rtc_set_time(r, 1995, 1, 1, 0, 0, 0, 0);
    return r;
}
