/*
 * pc486 - S3 ViRGE 2D/3D engine (MMIO)
 */
#ifndef S3ACCEL_H
#define S3ACCEL_H
#include <stdint.h>
#include <stdbool.h>

typedef struct S3Accel S3Accel;

S3Accel *s3a_new(uint8_t *vram, uint32_t vram_size, const uint8_t *cr, void *vga,
                 uint8_t (*vga_read)(void *, uint32_t),
                 void (*vga_write)(void *, uint32_t, uint8_t));
int s3a_busy(S3Accel *a);
void s3a_reset(S3Accel *a);
/* legacy MMIO window at A0000-AFFFF (enabled by CR53) */
bool s3a_mmio_hit_legacy(S3Accel *a, uint32_t addr);
/* addr: either a legacy A0000-AFFFF address or an offset >= 16MB inside the
 * 64MB linear window */
uint8_t s3a_mmio_read8(S3Accel *a, uint32_t addr);
uint16_t s3a_mmio_read16(S3Accel *a, uint32_t addr);
uint32_t s3a_mmio_read32(S3Accel *a, uint32_t addr);
void s3a_mmio_write8(S3Accel *a, uint32_t addr, uint8_t val);
void s3a_mmio_write16(S3Accel *a, uint32_t addr, uint16_t val);
void s3a_mmio_write32(S3Accel *a, uint32_t addr, uint32_t val);
#endif
