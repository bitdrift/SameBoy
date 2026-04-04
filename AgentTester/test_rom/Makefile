GBDK ?= /Users/ryan/Projects/gbdk
LCC = $(GBDK)/bin/lcc

ROM = harness_test.gb

CFLAGS = -Wm-yn"HARNESSTEST"

all: $(ROM)

$(ROM): main.c
	$(LCC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(ROM) *.o *.lst *.map *.sym *.ihx *.noi *.asm

.PHONY: all clean
