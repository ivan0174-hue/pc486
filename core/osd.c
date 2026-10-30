/*
 * pc486 - on-screen menu (Ctrl+Alt+F12): disk images, speed, reset
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "osd.h"
#include "pc.h"

extern const uint8_t vgafont16[256 * 16];

#define MAX_FILES 64
#define NAME_LEN 64

static PC *g_pc;
static const PCConfig *g_conf;
static char g_basedir[256] = ".";
static int g_active, g_reboot;
static int g_ctrl, g_alt;

static char g_fd_name[2][NAME_LEN];
static char g_cd_name[NAME_LEN];
static int g_cd_drive = -1;

/* menu state */
enum { M_MAIN, M_FILES };
static int g_menu, g_sel, g_target; /* target: 0/1 floppy, 2 cd */
static char g_files[MAX_FILES][NAME_LEN];
static int g_nfiles;
static char g_status[80];

/* kips = instructions per millisecond */
static const struct { const char *name; int kips; } speeds[] = {
    { "Massima", 0 },
    { "486DX4-100", 60000 },
    { "486DX2-66", 40000 },
    { "486DX-33", 20000 },
    { "386DX-25", 8000 },
    { "386SX-16", 2000 },
    { "286-12", 1000 },
};
#define SPEED_SLOW 5            /* preset used by Ctrl+Alt+F11 (turbo off) */
static int g_speed, g_speed_fast;
static char g_toast[80];
static uint32_t g_toast_t;

/* simple key=value option store filled from the ini file */
#define MAX_OPT 32
static char g_optk[MAX_OPT][32], g_optv[MAX_OPT][64];
static int g_nopt;

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

void osd_set_basedir(const char *dir)
{
    snprintf(g_basedir, sizeof(g_basedir), "%s", dir);
}

static int opt_handler(void *u, const char *sec, const char *name, const char *value)
{
    (void)u;
    if (g_nopt < MAX_OPT) {
        snprintf(g_optk[g_nopt], 32, "%s", name);
        snprintf(g_optv[g_nopt], 64, "%s", value);
        g_nopt++;
    }
    (void)sec;
    return 1;
}

#include "ini.h"
void osd_load_options(const char *inifile)
{
    g_nopt = 0;
    ini_parse(inifile, opt_handler, NULL);
}

const char *osd_get_option(const char *name)
{
    for (int i = 0; i < g_nopt; i++)
        if (!strcmp(g_optk[i], name))
            return g_optv[i];
    return NULL;
}

void osd_init(PC *pc, const PCConfig *conf)
{
    g_pc = pc;
    g_conf = conf;
    for (int i = 0; i < 2; i++)
        snprintf(g_fd_name[i], NAME_LEN, "%s",
                 conf->fd[i] && conf->fd[i][0] ? basename_of(conf->fd[i]) : "(vuoto)");
    for (int i = 0; i < 4; i++)
        if (conf->iscd[i]) {
            g_cd_drive = i;
            snprintf(g_cd_name, NAME_LEN, "%s",
                     conf->hd[i] && conf->hd[i][0] ? basename_of(conf->hd[i]) : "(vuoto)");
            break;
        }
    for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++)
        if (speeds[i].kips == conf->speed_kips)
            g_speed = i;
    g_speed_fast = g_speed == SPEED_SLOW ? 0 : g_speed;
}

int osd_active(void) { return g_active; }

static void set_speed(int i)
{
    g_speed = i;
    if (g_pc) {
        g_pc->speed_kips = speeds[i].kips;
        g_pc->speed_t0 = get_uticks();
        g_pc->speed_icount = 0;
    }
}

int osd_visible(void)
{
    if (g_active) return 1;
    if (g_toast[0] && get_uticks() - g_toast_t < 2500000) return 1;
    g_toast[0] = 0;
    return 0;
}
int osd_paused(void) { return g_active; }

int osd_take_reboot_request(void)
{
    int r = g_reboot;
    g_reboot = 0;
    return r;
}

static void open_menu(void)
{
    g_active = 1;
    g_menu = M_MAIN;
    g_sel = 0;
    g_status[0] = 0;
    /* release modifiers inside the guest */
    if (g_pc) {
        pc_key_event(g_pc, 0, 0x1d);
        pc_key_event(g_pc, 0, 0xe01d);
        pc_key_event(g_pc, 0, 0x38);
        pc_key_event(g_pc, 0, 0xe038);
    }
}

int osd_hotkey(int down, int set1)
{
    if (set1 == 0x1d || set1 == 0xe01d) g_ctrl = down;
    if (set1 == 0x38 || set1 == 0xe038) g_alt = down;
    if (down && set1 == 0x58 && g_ctrl && g_alt) {
        open_menu();
        return 1;
    }
    if (down && set1 == 0x57 && g_ctrl && g_alt && !g_active) {
        /* "turbo" button: slow 386 speed for old games and back */
        if (g_speed == SPEED_SLOW)
            set_speed(g_speed_fast);
        else {
            g_speed_fast = g_speed;
            set_speed(SPEED_SLOW);
        }
        snprintf(g_toast, sizeof(g_toast), " Turbo %s  (CPU: %s) ",
                 g_speed == SPEED_SLOW ? "SPENTO" : "ACCESO", speeds[g_speed].name);
        g_toast_t = get_uticks();
        return 1;
    }
    return 0;
}

static int has_ext(const char *n, const char *exts)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return 0;
    char e[12], list[80];
    int i = 0;
    e[i++] = ',';
    for (dot++; *dot && i < 9; dot++)
        e[i++] = (*dot >= 'A' && *dot <= 'Z') ? *dot + 32 : *dot;
    e[i++] = ',';
    e[i] = 0;
    snprintf(list, sizeof(list), ",%s,", exts);
    return strstr(list, e) != NULL;
}

static void load_files(int target)
{
    static char names[MAX_FILES][NAME_LEN];
    int n = plat_list_dir(g_basedir, names, MAX_FILES);
    const char *exts = target == 2 ? "iso,cdr" : "img,ima,vfd,flp,dsk";
    g_nfiles = 0;
    snprintf(g_files[g_nfiles++], NAME_LEN, "[ Espelli ]");
    for (int i = 0; i < n && g_nfiles < MAX_FILES; i++) {
        if (!has_ext(names[i], exts))
            continue;
        /* hide images already used as hard disks */
        int used = 0;
        for (int d = 0; d < 4; d++)
            if (g_conf->hd[d] && g_conf->hd[d][0] && !g_conf->iscd[d] &&
                !strcmp(basename_of(g_conf->hd[d]), names[i]))
                used = 1;
        if (!used)
            snprintf(g_files[g_nfiles++], NAME_LEN, "%s", names[i]);
    }
}

static void apply_file(int idx)
{
    char path[512];
    if (idx == 0) {
        if (g_target < 2) {
            fdc_eject(g_pc->fdc, g_target);
            snprintf(g_fd_name[g_target], NAME_LEN, "(vuoto)");
        } else if (g_cd_drive >= 0) {
            ide_change_cd(g_cd_drive < 2 ? g_pc->ide : g_pc->ide2, g_cd_drive & 1, "");
            snprintf(g_cd_name, NAME_LEN, "(vuoto)");
        }
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", g_basedir, g_files[idx]);
    if (g_target < 2) {
        if (fdc_insert(g_pc->fdc, g_target, path) == 0)
            snprintf(g_fd_name[g_target], NAME_LEN, "%s", g_files[idx]);
        else
            snprintf(g_status, sizeof(g_status), "Impossibile aprire %s", g_files[idx]);
    } else if (g_cd_drive >= 0) {
        ide_change_cd(g_cd_drive < 2 ? g_pc->ide : g_pc->ide2, g_cd_drive & 1, path);
        snprintf(g_cd_name, NAME_LEN, "%s", g_files[idx]);
    }
}

enum { I_FDA, I_FDB, I_CD, I_SPEED, I_SAVE, I_RESET, I_REBOOT, I_CLOSE, I_N };

static void main_line(int i, char *buf, int n)
{
    switch (i) {
    case I_FDA: snprintf(buf, n, "Floppy A:  %s", g_fd_name[0]); break;
    case I_FDB: snprintf(buf, n, "Floppy B:  %s", g_fd_name[1]); break;
    case I_CD:
        if (g_cd_drive >= 0) snprintf(buf, n, "CD-ROM:    %s", g_cd_name);
        else snprintf(buf, n, "CD-ROM:    (non configurato)");
        break;
    case I_SPEED: snprintf(buf, n, "Velocita': %s", speeds[g_speed].name); break;
    case I_SAVE: snprintf(buf, n, "Salva CMOS ora"); break;
    case I_RESET: snprintf(buf, n, "Reset del PC"); break;
    case I_REBOOT: snprintf(buf, n, "Riavvia il Raspberry Pi"); break;
    case I_CLOSE: snprintf(buf, n, "Chiudi menu (Esc)"); break;
    }
}

void osd_key(PC *pc, int down, int set1)
{
    (void)pc;
    if (set1 == 0x1d || set1 == 0xe01d) g_ctrl = down;
    if (set1 == 0x38 || set1 == 0xe038) g_alt = down;
    if (!down)
        return;
    int n = g_menu == M_MAIN ? I_N : g_nfiles;
    switch (set1) {
    case 0xe048: /* up */
        g_sel = (g_sel + n - 1) % n;
        return;
    case 0xe050: /* down */
        g_sel = (g_sel + 1) % n;
        return;
    case 0x01: /* esc */
        if (g_menu == M_FILES) {
            g_menu = M_MAIN;
            g_sel = g_target == 2 ? I_CD : g_target;
        } else
            g_active = 0;
        return;
    case 0x1c: case 0xe01c: /* enter */
        break;
    case 0xe04b: case 0xe04d: /* left/right change speed */
        if (g_menu == M_MAIN && g_sel == I_SPEED) {
            int ns = sizeof(speeds) / sizeof(speeds[0]);
            g_speed = (g_speed + (set1 == 0xe04d ? 1 : ns - 1)) % ns;
            set_speed(g_speed);
        }
        return;
    default:
        return;
    }
    if (g_menu == M_FILES) {
        apply_file(g_sel);
        g_menu = M_MAIN;
        g_sel = g_target == 2 ? I_CD : g_target;
        return;
    }
    switch (g_sel) {
    case I_FDA: case I_FDB: case I_CD:
        if (g_sel == I_CD && g_cd_drive < 0)
            return;
        g_target = g_sel == I_CD ? 2 : g_sel;
        load_files(g_target);
        g_menu = M_FILES;
        g_sel = 0;
        return;
    case I_SPEED: {
        int ns = sizeof(speeds) / sizeof(speeds[0]);
        g_speed = (g_speed + 1) % ns;
        set_speed(g_speed);
        return;
    }
    case I_SAVE:
        pc_save_nvram(g_pc, 1);
        snprintf(g_status, sizeof(g_status), "CMOS salvata");
        return;
    case I_RESET:
        pc_save_nvram(g_pc, 1);
        pc_reset(g_pc);
        g_active = 0;
        return;
    case I_REBOOT:
        g_reboot = 1;
        return;
    case I_CLOSE:
        g_active = 0;
        return;
    }
}

/* ---- drawing ---- */

static void draw_text(uint32_t *fb, int pitch, int fw, int fh, int x, int y,
                      const char *s, uint32_t fg, uint32_t bg)
{
    for (; *s; s++, x += 8) {
        if (x + 8 > fw || y + 16 > fh || x < 0 || y < 0)
            continue;
        const uint8_t *g = &vgafont16[(uint8_t)*s * 16];
        for (int r = 0; r < 16; r++) {
            uint32_t *d = fb + (y + r) * pitch + x;
            for (int c = 0; c < 8; c++)
                d[c] = (g[r] & (0x80 >> c)) ? fg : bg;
        }
    }
}

static void fill(uint32_t *fb, int pitch, int x, int y, int w, int h, uint32_t col)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            fb[(y + j) * pitch + x + i] = col;
}

void osd_draw(uint32_t *fb, int pitch, int w, int h)
{
    if (!g_active) {
        if (g_toast[0]) {
            int len = strlen(g_toast) * 8;
            int x = (w - len) / 2;
            draw_text(fb, pitch, w, h, x < 0 ? 0 : x, 8, g_toast, 0xffffff, 0x000080);
        }
        return;
    }
    int cols = w / 8 - 2;
    if (cols > 46) cols = 46;
    int rows = (g_menu == M_MAIN ? I_N : g_nfiles) + 4;
    int max_rows = h / 16 - 1;
    if (rows > max_rows) rows = max_rows;
    int bw = cols * 8 + 16, bh = rows * 16 + 8;
    int x0 = (w - bw) / 2, y0 = (h - bh) / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    const uint32_t bg = 0x000080, fg = 0xffffff, hl = 0x00aaaa, title = 0xffff55;
    fill(fb, pitch, x0, y0, bw, bh, bg);
    char line[128];
    draw_text(fb, pitch, w, h, x0 + 8, y0 + 4,
              g_menu == M_MAIN ? "pc486 - Menu  (Ctrl+Alt+F12)" :
              g_target == 2 ? "Scegli immagine CD" : "Scegli immagine floppy",
              title, bg);
    int n = g_menu == M_MAIN ? I_N : g_nfiles;
    int vis = rows - 4;
    int first = g_sel >= vis ? g_sel - vis + 1 : 0;
    for (int i = 0; i < vis && first + i < n; i++) {
        int k = first + i;
        if (g_menu == M_MAIN)
            main_line(k, line, sizeof(line));
        else
            snprintf(line, sizeof(line), "%s", g_files[k]);
        line[cols] = 0;
        char padded[128];
        snprintf(padded, sizeof(padded), "%-*s", cols, line);
        draw_text(fb, pitch, w, h, x0 + 8, y0 + 4 + (i + 2) * 16, padded,
                  k == g_sel ? 0x000000 : fg, k == g_sel ? hl : bg);
    }
    draw_text(fb, pitch, w, h, x0 + 8, y0 + 4 + (rows - 1) * 16,
              g_status[0] ? g_status : "Frecce, Invio, Esc", 0xaaaaaa, bg);
}

void osd_draw_message(uint32_t *fb, int pitch, int w, int h, const char *msg)
{
    fill(fb, pitch, 0, 0, w, h, 0x000040);
    int x = 16, y = 16;
    char line[128];
    int n = 0;
    for (const char *p = msg;; p++) {
        if (*p == '\n' || *p == 0 || n >= w / 8 - 4) {
            line[n] = 0;
            draw_text(fb, pitch, w, h, x, y, line, 0xffffff, 0x000040);
            y += 16;
            n = 0;
            if (!*p) break;
            if (*p != '\n') line[n++] = *p;
        } else
            line[n++] = *p;
    }
}
