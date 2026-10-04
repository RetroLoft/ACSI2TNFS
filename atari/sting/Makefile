# ACSI_NET.STX: STinG port driver for ACSI2TNFS (m68k-atari-mint-gcc, e.g. in WSL)
# -mshort: STinG passes 16-bit ints. No C library: only libgcc.
CC = m68k-atari-mint-gcc
CFLAGS = -m68000 -mshort -O2 -Wall -fomit-frame-pointer -fno-builtin -Iinclude
OBJS = init.o acsi_net.o acsinet.o

all: ACSI_NET.STX URLVIEW.TTP

ACSI_NET.STX: $(OBJS)
	$(CC) -mshort -nostdlib -o $@ $(OBJS) -lgcc -s

%.o: %.c include/port.h include/transprt.h acsinet.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

URLVIEW.TTP: start.o urlview.o
	$(CC) -mshort -nostdlib -o $@ start.o urlview.o -lgcc -s

clean:
	rm -f ACSI_NET.STX URLVIEW.TTP $(OBJS) start.o urlview.o
