# pc486 - Linux build (test front ends)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wno-unused-function -Wno-unused-variable
CFLAGS += -Icore -DI386_ENABLE_FPU -DUSE_FMOPL -MMD -MP

CORE = core/i386.c core/fpu.c core/i8259.c core/i8254.c core/i8257.c \
       core/i8042.c core/ide.c core/pci.c core/pcspk.c core/adlib.c \
       core/fmopl.c core/sb16.c core/ini.c core/rtc.c core/fdc.c \
       core/uart.c core/keymap_hid.c core/s3virge.c core/s3accel.c core/pcibus.c core/pc.c core/config.c core/osd.c core/vgafont.c

OBJS = $(CORE:.c=.o)

all: pc486_headless

pc486_headless: $(OBJS) linux/hal_linux.o linux/headless.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

core/i386.o: core/i386.c core/i386ins.def core/simd.inc
	$(CC) $(CFLAGS) -c $< -o $@

pc486_trace: $(CORE) linux/hal_linux.c linux/headless.c
	$(CC) $(CFLAGS) -DPC486_TRACE -o $@ $^ -lm

-include $(OBJS:.o=.d) linux/hal_linux.d linux/headless.d

clean:
	rm -f core/*.o core/*.d linux/*.o linux/*.d pc486_headless pc486_trace
