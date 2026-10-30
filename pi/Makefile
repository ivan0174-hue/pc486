#
# pc486 - Raspberry Pi 4 (AArch64) bare-metal build with Circle + clang
#
#   make CIRCLEHOME=/path/to/circle
#
# Circle must be configured with:  AARCH=64 RASPPI=4 CLANG=1 STDLIB_SUPPORT=0
# and its libraries plus addon/SDCard and addon/fatfs must be built.
#
CIRCLEHOME ?= ../../circle
CORE = ../core
LIBMDIR = libm
BUILD = build

CORE_SRCS = i386.c fpu.c i8259.c i8254.c i8257.c i8042.c ide.c pci.c pcspk.c \
	    adlib.c fmopl.c sb16.c ini.c rtc.c fdc.c uart.c keymap_hid.c \
	    s3virge.c s3accel.c pcibus.c pc.c config.c osd.c vgafont.c
LIBM_SRCS = $(notdir $(wildcard $(LIBMDIR)/*.c))

OBJS = main.o kernel.o bootlog.o libc_shim.o \
       $(addprefix $(BUILD)/,$(CORE_SRCS:.c=.o)) \
       $(addprefix $(BUILD)/m_,$(LIBM_SRCS:.c=.o))

LIBS = $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/sound/libsound.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/libcircle.a

CLANG_RES := $(shell clang -print-resource-dir)
EXTRAINCLUDE += -I $(CURDIR)/include -isystem $(CLANG_RES)/include -I $(CURDIR)/$(CORE)
DEFINE += -DI386_ENABLE_FPU -DUSE_FMOPL -DPC486_BAREMETAL

EXTRACLEAN = $(BUILD)/*.o $(BUILD)/*.d

include $(CIRCLEHOME)/Rules.mk

# the emulator core is speed critical
$(BUILD)/%.o: $(CORE)/%.c
	@mkdir -p $(BUILD)
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) $(C_STANDARD) -O3 -Wno-unused-function -Wno-unused-variable \
		-Wno-unused-but-set-variable -Wno-sometimes-uninitialized -MMD -MP -c -o $@ $<

$(BUILD)/m_%.o: $(LIBMDIR)/%.c
	@mkdir -p $(BUILD)
	@echo "  CC    $@"
	@$(CC) $(ARCH) -O2 -ffreestanding -nostdinc -isystem $(CLANG_RES)/include \
		-I $(LIBMDIR) -I include -w -c -o $@ $<

-include $(DEPS)
-include $(wildcard $(BUILD)/*.d)
