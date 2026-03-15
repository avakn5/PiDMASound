PROGS := songs/mario.c # default: Mario theme
COMMON_SRC := asm/interrupts-asm.S src/sigma-delta.c src/audio-dma.c
OUR_START := asm/start.S
MEMMAP := ./asm/memmap

OPT_LEVEL = -O2
CFLAGS += -Isrc

TTYUSB =
BOOTLOADER = pi-install

include $(CS140E_2026_PATH)/libpi/mk/Makefile.robust-v2

mario: all

starwars:
	$(MAKE) PROGS=songs/starwars.c

harry:
	$(MAKE) PROGS=songs/harry.c

beethoven:
	$(MAKE) PROGS=songs/beethoven.c

nokia:
	$(MAKE) PROGS=songs/nokia.c

sinewave:
	$(MAKE) PROGS=songs/sinewave.c

debug-sinewave: sinewave

clear: clean
	rm -rf objs
