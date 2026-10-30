#ifndef PC486_PCIBUS_H
#define PC486_PCIBUS_H
#include <stdint.h>

typedef struct {
    uint32_t addr;
    uint8_t host[256];
    uint8_t virge[256];
    void *vga_opaque;
    void (*set_lfb)(void *opaque, uint32_t base, int enabled);
} PCIBusState;

void pcibus_init(PCIBusState *p, void *vga_opaque,
                 void (*set_lfb)(void *opaque, uint32_t base, int enabled));
uint32_t pcibus_read(PCIBusState *p, int port, int size);
void pcibus_write(PCIBusState *p, int port, uint32_t val, int size);

#endif
