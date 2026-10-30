/*
 * pc486 - S3 ViRGE (86C325) + VGA core emulation
 */
#ifndef S3VIRGE_H
#define S3VIRGE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct S3VState S3VState;

/* vram_size in bytes (2 MB or 4 MB) */
S3VState *s3v_new(uint32_t vram_size);
void s3v_reset(S3VState *s);
void s3v_set_pci_base(S3VState *s, uint32_t base, int enabled);

/* I/O ports 3B0-3DF (8 bit) */
uint8_t s3v_io_read(S3VState *s, uint32_t port);
void s3v_io_write(S3VState *s, uint32_t port, uint8_t val);

/* legacy window A0000-BFFFF: addr is the full physical address */
uint8_t s3v_mem_read8(S3VState *s, uint32_t addr);
void s3v_mem_write8(S3VState *s, uint32_t addr, uint8_t val);
uint16_t s3v_mem_read16(S3VState *s, uint32_t addr);
void s3v_mem_write16(S3VState *s, uint32_t addr, uint16_t val);
uint32_t s3v_mem_read32(S3VState *s, uint32_t addr);
void s3v_mem_write32(S3VState *s, uint32_t addr, uint32_t val);

/* linear aperture: returns true if addr belongs to the card */
bool s3v_lfb_hit(S3VState *s, uint32_t addr);
uint8_t s3v_lfb_read8(S3VState *s, uint32_t addr);
void s3v_lfb_write8(S3VState *s, uint32_t addr, uint8_t val);
uint16_t s3v_lfb_read16(S3VState *s, uint32_t addr);
void s3v_lfb_write16(S3VState *s, uint32_t addr, uint16_t val);
uint32_t s3v_lfb_read32(S3VState *s, uint32_t addr);
void s3v_lfb_write32(S3VState *s, uint32_t addr, uint32_t val);

/* Render the current frame into an XRGB8888 buffer of at least
 * S3V_MAX_W * S3V_MAX_H pixels; returns false if nothing changed since the
 * last call (when allowed to skip). Width/height of the image are returned. */
#define S3V_MAX_W 1600
#define S3V_MAX_H 1200
bool s3v_render(S3VState *s, uint32_t *fb, int *w, int *h, int *pitch_pixels);

/* call periodically: handles retrace timing */
void s3v_step(S3VState *s);

/* debug */
void s3v_dump_state(S3VState *s);
int s3v_text_dump(S3VState *s, char *out, int maxlen);
const char *s3v_mode_desc(S3VState *s);

#endif
