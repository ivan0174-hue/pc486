# Componenti e licenze

pc486 riunisce codice scritto per questo progetto e codice di terze parti.

| Parte | Origine | Licenza |
|---|---|---|
| `core/i386.c`, `fpu.c`, `simd.inc`, `i386ins.def` (CPU) | tiny386, © 2024-2025 Chunhui He (commit in `core/TINY386_COMMIT`), modificati: linea A20, scritture in area ROM, reset su triple fault | BSD 3-clause (`core/LICENSE`) |
| `core/i8259.c`, `i8254.c`, `i8257.c`, `i8042.c`, `ide.c`, `pci.c`, `pcspk.c`, `adlib.c`, `sb16.c` | QEMU / TinyEMU (F. Bellard, V. Karpov e altri) tramite tiny386; `i8042.c` e `ide.c` modificati (comandi AMI, A20, task file condiviso, comandi ATA mancanti, geometria CHS) | MIT |
| `core/fmopl.c` | MAME 0.37a, T. Satoh (via QEMU/tiny386) | LGPL |
| `core/ini.c` | inih, Ben Hoyt | BSD |
| `core/vgafont.c` | font VGA 8×16 da tiny386 | BSD 3-clause |
| `core/s3virge.c`, `s3accel.c`, `pcibus.c`, `pcibios_rom.h`, `rtc.c`, `fdc.c`, `uart.c`, `pc.c`, `config.c`, `osd.c`, `keymap_hid.c`, `pi/`, `linux/`, `tools/` | scritti per pc486 | BSD 3-clause |
| `pi/libm/` | OpenLibm (FreeBSD msun) | MIT/BSD (`pi/libm/LICENSE.md`) |
| Circle (linkato nel kernel del Pi) | R. Stange, github.com/rsta2/circle | **GPL v3** |
| Firmware del Raspberry Pi (`start4.elf`, `fixup4.dat`, `.dtb`) | Raspberry Pi Ltd | `LICENCE.broadcom` |
| FreeDOS (`freedos.img`, `freedos_hd.img`) | The FreeDOS Project, sorgenti su freedos.org | GPL v2 |

Poiché il kernel per il Raspberry Pi è linkato con Circle, il file
`kernel8-rpi4.img` nel suo insieme va distribuito secondo la GPL v3.

I BIOS `ami486.bin` (American Megatrends, 1993) e `s3virge.bin` (Diamond
Multimedia / S3, 1996) sono stati forniti dall'utente e restano dei rispettivi
titolari dei diritti.
