#!/bin/sh
# pc486 - build the Raspberry Pi 4 kernel and assemble dist/sdcard
# Requirements: clang (>= 15), lld, llvm-objcopy, llvm-ar, git, make, python3
set -e
TOP=$(cd "$(dirname "$0")/.." && pwd)
CIRCLE=${CIRCLE:-$TOP/../circle}
CIRCLE_COMMIT=7a81e1b480fe7d19fc1fc695b89bff0a0848f44b
FW_COMMIT=0641c5bf25d185d5bf25d8dcafd1da6dc8fdbf68
RES=$(clang -print-resource-dir)

if [ ! -d "$CIRCLE" ]; then
	git clone https://github.com/rsta2/circle "$CIRCLE"
	git -C "$CIRCLE" checkout -q $CIRCLE_COMMIT
fi

cat > "$CIRCLE/Config.mk" <<EOF
AARCH = 64
RASPPI = 4
CLANG = 1
STDLIB_SUPPORT = 0
DEFINE += -DARM_ALLOW_MULTI_CORE
EOF

(cd "$CIRCLE" && ./makeall --nosample -j"$(nproc)")
make -C "$CIRCLE/addon/SDCard" -j"$(nproc)"
make -C "$CIRCLE/addon/fatfs" -j"$(nproc)" \
	EXTRAINCLUDE="-I $TOP/pi/include -isystem $RES/include"

# armstub for the Pi 4 (assembled with clang)
(cd "$CIRCLE/boot/armstub" &&
	clang --target=aarch64-none-elf -DGIC=1 -c armstub8.S -o armstub8-rpi4.o &&
	ld.lld --section-start=.text=0 -o armstub8-rpi4.elf armstub8-rpi4.o &&
	llvm-objcopy armstub8-rpi4.elf -O binary armstub8-rpi4.bin)

make -C "$TOP/pi" CIRCLEHOME="$CIRCLE" -j"$(nproc)"

# firmware files
FW="$TOP/../rpi-firmware"
if [ ! -f "$FW/boot/start4.elf" ]; then
	mkdir -p "$FW" && cd "$FW"
	git init -q
	git remote add origin https://github.com/raspberrypi/firmware || true
	git config core.sparseCheckout true
	printf "boot/start4.elf\nboot/fixup4.dat\nboot/bcm2711-rpi-4-b.dtb\nboot/bcm2711-rpi-400.dtb\nboot/bcm2711-rpi-cm4.dtb\nboot/LICENCE.broadcom\n" > .git/info/sparse-checkout
	git fetch -q --depth 1 --filter=blob:none origin $FW_COMMIT
	git checkout -q FETCH_HEAD
fi

D="$TOP/dist/sdcard"
mkdir -p "$D/pc486"
cp "$FW"/boot/start4.elf "$FW"/boot/fixup4.dat "$FW"/boot/bcm2711-*.dtb "$FW"/boot/LICENCE.broadcom "$D/"
cp "$CIRCLE/boot/armstub/armstub8-rpi4.bin" "$TOP/pi/kernel8-rpi4.img" "$D/"
cp "$TOP/pi/sdcard/config.txt" "$TOP/pi/sdcard/cmdline.txt" "$D/"
[ -f "$D/pc486/pc486.ini" ] || cp "$TOP/pi/sdcard/pc486.ini" "$D/pc486/"
echo
echo "Done: copy the content of $D to a FAT32 SD card,"
echo "then put ami486.bin and s3virge.bin into the pc486 folder."
