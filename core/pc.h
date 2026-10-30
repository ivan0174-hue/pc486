/*
 * pc486 - AMI 486 / ALi M1429 PC with S3 ViRGE
 */
#ifndef PC_H
#define PC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "i386.h"
#include "i8259.h"
#include "i8254.h"
#include "i8257.h"
#include "i8042.h"
#include "ide.h"
#include "pcspk.h"
#include "adlib.h"
#include "sb16.h"
#include "rtc.h"
#include "fdc.h"
#include "s3virge.h"
#include "uart.h"
#include "pcibus.h"

/* ---- platform HAL (implemented by each front end) ---- */
uint32_t get_uticks(void);                      /* monotonic microseconds */
void *bigmalloc(size_t size);
/* reads a whole file into buf (up to maxlen), returns length or -1 */
long plat_load_file(const char *path, void *buf, long maxlen);
/* writes a whole file, returns 0 on success */
int plat_save_file(const char *path, const void *buf, long len);
void plat_log(const char *fmt, ...);
/* list file names in a directory (no subdirectories); returns count */
int plat_list_dir(const char *dir, char names[][64], int max);

typedef struct {
    const char *bios;       /* system BIOS image (64K or 128K) */
    const char *vga_bios;   /* video BIOS (placed at C0000) */
    const char *nvram;      /* CMOS save file (may be NULL) */
    long mem_size;          /* bytes */
    long vram_size;         /* bytes (2M or 4M) */
    const char *hd[4];      /* hda..hdd image paths */
    int iscd[4];
    const char *fd[2];      /* floppy images */
    int cpu_gen;            /* 4 = 486 */
    int fpu;
    int speed_kips;         /* 0 = unlimited, else instructions per millisecond */
    int sound;              /* enable SB16 + OPL */
    int serial_mouse;       /* Microsoft serial mouse on COM1 */
    int pci;                /* PCI bus with the ViRGE + PCI BIOS extension */
} PCConfig;

typedef struct PC {
    CPUI386 *cpu;
    PicState2 *pic;
    PITState *pit;
    RTC *rtc;
    KBDState *kbc;
    PS2KbdState *kbd;
    PS2MouseState *mouse;
    I8257State *dma, *hdma;
    FDC *fdc;
    IDEIFState *ide, *ide2;
    S3VState *vga;
    PCSpkState *spk;
    AdlibState *adlib;
    SB16State *sb16;
    UART *com1, *com2;
    PCIBusState pcibus;
    int has_pci;

    uint8_t *ram;
    long ram_size;

    /* ISA ROM space C0000-FFFFF and shadow RAM for it */
    uint8_t rom[0x40000];
    uint8_t shadow[0x40000];
    uint8_t blk_read_ram[8];
    uint8_t blk_write_ram[8];

    /* ALi M1429 */
    uint8_t ali_idx;
    uint8_t ali[256];

    uint8_t port80[16];
    uint8_t post_code;
    uint8_t port92;
    int reset_request;
    int post_log;
    int trace_lo, trace_hi;
    int speed_kips;
    uint32_t speed_t0;
    uint64_t speed_icount;

    int rep_key;
    uint32_t rep_time;

    uint32_t audio_ext_t;   /* last audio pull by the front end */
    uint32_t audio_int_t;   /* last internal (silent) pull */

    const PCConfig *conf;
    uint32_t nvram_save_t;
    int nvram_pending;
} PC;

PC *pc_new(const PCConfig *conf);
void pc_reset(PC *pc);           /* hard reset (power on) */
/* run for about `us` microseconds of wall-clock time or `steps` slices */
void pc_step(PC *pc);
void pc_save_nvram(PC *pc, int force);

/* input */
void pc_key_event(PC *pc, int is_down, int keycode); /* PC/AT set-1 code, 0xe0xx for extended */
void pc_mouse_event(PC *pc, int dx, int dy, int dz, int buttons);

/* audio: fill `len` bytes of s16 stereo at 44100 Hz */
void pc_audio_callback(PC *pc, uint8_t *stream, int len);

#endif
