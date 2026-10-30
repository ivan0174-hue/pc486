/*
 * pc486 - headless Linux front end used for testing.
 *
 *   pc486_headless [options] config.ini
 *     -t SEC            stop after SEC seconds
 *     -k SEC:KEY        press KEY at time SEC (names: ESC ENTER DEL F1..F12 UP ...
 *                       or single characters); repeatable
 *     -T SEC:text       type text at time SEC
 *     -s SEC:file.ppm   save a screenshot at time SEC; repeatable
 *     -p                log POST codes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pc.h"
#include "config.h"
#include "osd.h"

typedef struct { double t; int kind; char arg[256]; int done; } Ev;
static Ev evs[256];
static int nev;

static const struct { const char *name; int code; } keys[] = {
    {"ESC",1},{"BKSP",0x0e},{"TAB",0x0f},{"ENTER",0x1c},{"CTRL",0x1d},
    {"LSHIFT",0x2a},{"RSHIFT",0x36},{"ALT",0x38},{"SPACE",0x39},{"CAPS",0x3a},
    {"F1",0x3b},{"F2",0x3c},{"F3",0x3d},{"F4",0x3e},{"F5",0x3f},{"F6",0x40},
    {"F7",0x41},{"F8",0x42},{"F9",0x43},{"F10",0x44},{"F11",0x57},{"F12",0x58},
    {"HOME",0xe047},{"UP",0xe048},{"PGUP",0xe049},{"LEFT",0xe04b},{"RIGHT",0xe04d},
    {"END",0xe04f},{"DOWN",0xe050},{"PGDN",0xe051},{"INS",0xe052},{"DEL",0xe053},{"KPDEL",0x53},
    {NULL,0}
};

static const char *row1 = "1234567890-=";
static const char *rowq = "qwertyuiop[]";
static const char *rowa = "asdfghjkl;'`";
static const char *rowz = "zxcvbnm,./";

static int char_code(int ch, int *shift)
{
    const char *p;
    static const char *sh1 = "!@#$%^&*()_+";
    static const char *shq = "QWERTYUIOP{}";
    static const char *sha = "ASDFGHJKL:\"~";
    static const char *shz = "ZXCVBNM<>?";
    *shift = 0;
    if (ch == ' ') return 0x39;
    if (ch == '\n') return 0x1c;
    if (ch == '\\') return 0x2b;
    if (ch == '|') { *shift = 1; return 0x2b; }
    if ((p = strchr(row1, ch))) return 0x02 + (p - row1);
    if ((p = strchr(rowq, ch))) return 0x10 + (p - rowq);
    if ((p = strchr(rowa, ch))) return 0x1e + (p - rowa);
    if ((p = strchr(rowz, ch))) return 0x2c + (p - rowz);
    *shift = 1;
    if ((p = strchr(sh1, ch))) return 0x02 + (p - sh1);
    if ((p = strchr(shq, ch))) return 0x10 + (p - shq);
    if ((p = strchr(sha, ch))) return 0x1e + (p - sha);
    if ((p = strchr(shz, ch))) return 0x2c + (p - shz);
    *shift = 0;
    return 0;
}

static int key_code(const char *name, int *shift)
{
    *shift = 0;
    for (int i = 0; keys[i].name; i++)
        if (!strcasecmp(keys[i].name, name))
            return keys[i].code;
    if (strlen(name) == 1)
        return char_code(name[0], shift);
    return 0;
}

static uint32_t *fb;
static PC *g_pc;
void pc486_de_hook(void)
{
    static int done;
    if (done++ || !getenv("DUMPRAM")) return;
    FILE *df = fopen(getenv("DUMPRAM"), "wb");
    if (df) { fwrite(g_pc->ram, 1, 1 << 20, df); fclose(df); }
}

static void screenshot(PC *pc, const char *file)
{
    int w, h, pitch;
    s3v_render(pc->vga, fb, &w, &h, &pitch);
    if (osd_visible()) osd_draw(fb, pitch, w, h);
    FILE *f = fopen(file, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = fb[y * pitch + x];
            uint8_t rgb[3] = { c >> 16, c >> 8, c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "[%s] screenshot %s (%s)\n", file, file, s3v_mode_desc(pc->vga));
}

/* typed key presses are queued so that each gets its own time slot */
typedef struct { int code; int down; } KQ;
static KQ kq[4096];
static int kq_h, kq_t;
static void kq_push(int code, int down) { kq[kq_t] = (KQ){code, down}; kq_t = (kq_t + 1) % 4096; }

#ifdef PC486_TRACE
uint32_t pc486_trace_buf[65536][2];
unsigned pc486_trace_pos;
int pc486_trace_far = 1;
static void dump_trace(int n)
{
    uint32_t lastcs = 0xffffffff;
    for (unsigned i = pc486_trace_pos - n; i != pc486_trace_pos; i++) {
        uint32_t *e = pc486_trace_buf[i & 65535];
        fprintf(stderr, "%04x:%04x %02x%s\n", e[0] & 0xffff, e[1], (e[0] >> 16) & 0xff, (e[0] & 0x1000000) ? " P" : "");
        (void)lastcs;
    }
}
#endif

int main(int argc, char **argv)
{
    double tmax = 30;
    int post = 0;
    int trace_lo = 0x10000, trace_hi = 0;
    const char *ini = NULL;
    const char *wavname = NULL;
    FILE *wav = NULL;
    uint64_t wav_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) tmax = atof(argv[++i]);
        else if (!strcmp(argv[i], "-p")) post = 1;
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) wavname = argv[++i];
        else if (!strcmp(argv[i], "-sd") && i + 1 < argc) { extern const char *plat_sd_image; plat_sd_image = argv[++i]; }
        else if (!strcmp(argv[i], "-io") && i + 1 < argc) { sscanf(argv[++i], "%x-%x", &trace_lo, &trace_hi); }
        else if ((!strcmp(argv[i], "-k") || !strcmp(argv[i], "-s") || !strcmp(argv[i], "-T") || !strcmp(argv[i], "-d")) && i + 1 < argc) {
            char *a = argv[++i];
            char *c = strchr(a, ':');
            if (!c && argv[i - 1][1] == 'd') c = a + strlen(a);
            if (!c) continue;
            evs[nev].t = atof(a);
            evs[nev].kind = argv[i - 1][1];
            snprintf(evs[nev].arg, sizeof(evs[nev].arg), "%s", *c ? c + 1 : "");
            nev++;
        } else ini = argv[i];
    }
    PCConfig conf;
    pc_config_defaults(&conf);
    if (ini) {
        char base[1024];
        snprintf(base, sizeof(base), "%s", ini);
        char *sl = strrchr(base, '/');
        if (sl) *sl = 0; else strcpy(base, ".");
        if (pc_config_load(&conf, ini, base) < 0) {
            fprintf(stderr, "cannot read %s\n", ini);
            return 1;
        }
    }
    fb = malloc(S3V_MAX_W * S3V_MAX_H * 4);
    PC *pc = pc_new(&conf);
    if (!pc) return 1;
    g_pc = pc;
    pc->post_log = post;
    {
        char base[1024];
        snprintf(base, sizeof(base), "%s", ini ? ini : ".");
        char *sl = strrchr(base, '/');
        if (sl) *sl = 0; else strcpy(base, ".");
        osd_set_basedir(base);
        if (ini) osd_load_options(ini);
        osd_init(pc, &conf);
    }
    pc->trace_lo = trace_lo;
    pc->trace_hi = trace_hi;

    uint32_t t0 = get_uticks();
    uint32_t next_key = get_uticks();
    if (wavname) {
        wav = fopen(wavname, "wb");
        if (wav) fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data\0\0\0\0", 1, 44, wav);
    }
    for (;;) {
        if (!osd_paused()) pc_step(pc);
        double t = (get_uticks() - t0) / 1e6;
        if (t > tmax) break;
        if (wav) {
            /* pull audio in real time like the Pi front end (10 ms blocks) */
            uint64_t want = (uint64_t)(t * 44100);
            while (want - wav_frames >= 441) {
                static uint8_t ab[441 * 4];
                pc_audio_callback(pc, ab, sizeof(ab));
                fwrite(ab, 1, sizeof(ab), wav);
                wav_frames += 441;
            }
        }
        for (int i = 0; i < nev; i++) {
            if (evs[i].done || t < evs[i].t) continue;
            evs[i].done = 1;
            if (evs[i].kind == 's') {
                screenshot(pc, evs[i].arg);
            } else if (evs[i].kind == 'd') {
                static char txt[16384];
                s3v_text_dump(pc->vga, txt, sizeof(txt));
                fprintf(stderr, "---- screen @%.1fs [%s] POST=%02x ----\n%s----\n", t, s3v_mode_desc(pc->vga), pc->post_code, txt);
            } else if (evs[i].kind == 'k') {
                char name[64];
                snprintf(name, sizeof(name), "%s", evs[i].arg);
                int mode = 0, l = strlen(name);
                if (l > 1 && name[l - 1] == '+') { mode = 1; name[l - 1] = 0; }
                else if (l > 1 && name[l - 1] == '-') { mode = 2; name[l - 1] = 0; }
                int sh, code = key_code(name, &sh);
                if (!code) { fprintf(stderr, "unknown key %s\n", evs[i].arg); continue; }
                if (sh && mode != 2) kq_push(0x2a, 1);
                if (mode != 2) kq_push(code, 1);
                if (mode != 1) kq_push(code, 0);
                if (sh && mode != 1) kq_push(0x2a, 0);
            } else if (evs[i].kind == 'T') {
                for (char *p = evs[i].arg; *p; p++) {
                    int sh, code;
                    if (*p == '~' && p[1] == 'n') { code = 0x1c; sh = 0; p++; }
                    else code = char_code(*p, &sh);
                    if (!code) continue;
                    if (sh) kq_push(0x2a, 1);
                    kq_push(code, 1);
                    kq_push(code, 0);
                    if (sh) kq_push(0x2a, 0);
                }
            }
        }
        if (kq_h != kq_t && (int32_t)(get_uticks() - next_key) >= 0) {
            if (osd_active() || osd_hotkey(kq[kq_h].down, kq[kq_h].code)) {
                if (osd_active()) osd_key(pc, kq[kq_h].down, kq[kq_h].code);
            } else
                pc_key_event(pc, kq[kq_h].down, kq[kq_h].code);
            if (pc->trace_lo <= 0x60) fprintf(stderr, "[%.3f] key %x %s\n", t, kq[kq_h].code, kq[kq_h].down ? "down" : "up");
            kq_h = (kq_h + 1) % 4096;
            next_key = get_uticks() + 30000;
        }
    }
    {
        CPUI386_State st;
        cpui386_get_state(pc->cpu, &st);
        fprintf(stderr, "CPU: CS:IP=%04x:%08x FL=%08x EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x ESP=%08x SS=%04x DS=%04x ES=%04x CR0=%08x halt=%d\n",
                st.seg[1], st.ip, st.flags, st.gpr[0], st.gpr[3], st.gpr[1], st.gpr[2], st.gpr[6], st.gpr[7], st.gpr[4], st.seg[2], st.seg[3], st.seg[0], st.cr0, cpui386_is_halted(pc->cpu));
    }
#ifdef PC486_TRACE
    if (getenv("TRACE_N")) dump_trace(atoi(getenv("TRACE_N")));
#endif
    if (wav) {
        uint32_t n = wav_frames * 4, r = n + 36;
        fseek(wav, 4, SEEK_SET); fwrite(&r, 4, 1, wav);
        fseek(wav, 40, SEEK_SET); fwrite(&n, 4, 1, wav);
        fclose(wav);
    }
    pc_save_nvram(pc, 1);
    fprintf(stderr, "instructions: %ld (%.1f MIPS avg)\n", cpui386_get_cycle(pc->cpu),
            cpui386_get_cycle(pc->cpu) / ((get_uticks() - t0) / 1e6) / 1e6);
    fprintf(stderr, "end: POST=%02x mode=%s\n", pc->post_code, s3v_mode_desc(pc->vga));
    return 0;
}
