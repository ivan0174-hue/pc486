#ifndef FDC_H
#define FDC_H
#include <stdint.h>

typedef struct FDC FDC;
FDC *fdc_new(int irq, int dma_chan, void *pic,
             void (*set_irq)(void *pic, int irq, int level), void *dma);
uint8_t fdc_ioport_read(FDC *f, int port);
void fdc_ioport_write(FDC *f, int port, uint8_t val);
void fdc_step(FDC *f);
int fdc_insert(FDC *f, int drive, const char *path);
void fdc_eject(FDC *f, int drive);
int fdc_has_disk(FDC *f, int drive);

#endif
