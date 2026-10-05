# NETTEST.TTP: ACSI_NET test tool (m68k-atari-mint-gcc, e.g. in WSL)
CC = m68k-atari-mint-gcc
CFLAGS = -m68000 -O2 -Wall -fomit-frame-pointer

NETTEST.TTP: nettest.c
	$(CC) $(CFLAGS) -o $@ $< -s

clean:
	rm -f NETTEST.TTP
