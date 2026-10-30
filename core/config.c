/* pc486 - configuration file (INI) handling shared by all front ends */
#include <stdlib.h>
#include <string.h>
#include "pc.h"
#include "config.h"
#include "ini.h"

static long parse_size(const char *v)
{
    long a = atol(v);
    int n = strlen(v);
    if (n) {
        switch (v[n - 1]) {
        case 'G': case 'g': a *= 1024L * 1024 * 1024; break;
        case 'M': case 'm': a *= 1024L * 1024; break;
        case 'K': case 'k': a *= 1024L; break;
        }
    }
    return a;
}

/* "2.5" -> 2500 (no floating point library needed) */
static int parse_milli(const char *v)
{
    int r = 0, frac = 0, scale = 1000;
    while (*v == ' ') v++;
    for (; *v >= '0' && *v <= '9'; v++) r = r * 10 + (*v - '0');
    if (*v == '.' || *v == ',')
        for (v++; *v >= '0' && *v <= '9' && scale > 1; v++) {
            scale /= 10;
            frac += (*v - '0') * scale;
        }
    return r * 1000 + frac;
}

static char *dup_path(const char *base, const char *v)
{
    if (!v[0])
        return strdup("");
    if (v[0] == '/' || !base || !base[0] || strchr(v, ':'))
        return strdup(v);
    char *r = malloc(strlen(base) + strlen(v) + 2);
    strcpy(r, base);
    strcat(r, "/");
    strcat(r, v);
    return r;
}

typedef struct { PCConfig *c; const char *base; } Ctx;

static int handler(void *user, const char *sec, const char *name, const char *value)
{
    Ctx *x = user;
    PCConfig *c = x->c;
#define S(a) (strcmp(sec, a) == 0)
#define N(a) (strcmp(name, a) == 0)
    if (S("pc")) {
        if (N("bios")) c->bios = dup_path(x->base, value);
        else if (N("vga_bios")) c->vga_bios = dup_path(x->base, value);
        else if (N("nvram")) c->nvram = dup_path(x->base, value);
        else if (N("mem_size")) c->mem_size = parse_size(value);
        else if (N("vga_mem_size") || N("vram_size")) c->vram_size = parse_size(value);
        else if (N("hda")) { c->hd[0] = dup_path(x->base, value); c->iscd[0] = 0; }
        else if (N("hdb")) { c->hd[1] = dup_path(x->base, value); c->iscd[1] = 0; }
        else if (N("hdc")) { c->hd[2] = dup_path(x->base, value); c->iscd[2] = 0; }
        else if (N("hdd")) { c->hd[3] = dup_path(x->base, value); c->iscd[3] = 0; }
        else if (N("cda")) { c->hd[0] = dup_path(x->base, value); c->iscd[0] = 1; }
        else if (N("cdb")) { c->hd[1] = dup_path(x->base, value); c->iscd[1] = 1; }
        else if (N("cdc")) { c->hd[2] = dup_path(x->base, value); c->iscd[2] = 1; }
        else if (N("cdd")) { c->hd[3] = dup_path(x->base, value); c->iscd[3] = 1; }
        else if (N("fda")) c->fd[0] = dup_path(x->base, value);
        else if (N("fdb")) c->fd[1] = dup_path(x->base, value);
        else if (N("sound")) c->sound = atoi(value);
        else if (N("serial_mouse")) c->serial_mouse = atoi(value);
        else if (N("pci")) c->pci = atoi(value);
    } else if (S("cpu")) {
        if (N("gen")) c->cpu_gen = atoi(value);
        else if (N("fpu")) c->fpu = atoi(value);
        else if (N("speed") || N("mips")) c->speed_kips = parse_milli(value);
    }
#undef S
#undef N
    return 1;
}

void pc_config_defaults(PCConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->bios = "ami486.bin";
    c->vga_bios = "s3virge.bin";
    c->nvram = "cmos.bin";
    c->mem_size = 16L << 20;
    c->vram_size = 4L << 20;
    c->cpu_gen = 4;
    c->fpu = 1;
    c->sound = 1;
    c->serial_mouse = 1;
    c->pci = 1;
}

int pc_config_load(PCConfig *c, const char *path, const char *basedir)
{
    Ctx x = { c, basedir };
    return ini_parse(path, handler, &x);
}
