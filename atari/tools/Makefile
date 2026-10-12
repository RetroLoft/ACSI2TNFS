# NETTEST.TTP: ACSI_NET test tool (m68k-atari-mint-gcc, e.g. in WSL)
CC = m68k-atari-mint-gcc
CFLAGS = -m68000 -O2 -Wall -fomit-frame-pointer

NETTEST.TTP: nettest.c
	$(CC) $(CFLAGS) -o $@ $< -s
	m68k-atari-mint-flags -q --mno-altram --mno-altalloc $@   # ACSI DMA: ST-RAM only (TT)

# NETCLOCK.PRG: like ACSI_NET.STX, -mshort without C library
SFLAGS = -m68000 -mshort -O2 -Wall -fomit-frame-pointer -fno-builtin -I../sting/include
NETCLOCK.PRG: ../sting/init.S netclock.c ../sting/acsinet.c ../sting/acsinet.h
	$(CC) $(SFLAGS) -c ../sting/init.S -o netclock_init.o
	$(CC) $(SFLAGS) -c netclock.c -o netclock.o
	$(CC) $(SFLAGS) -c ../sting/acsinet.c -o netclock_acsi.o
	$(CC) -mshort -nostdlib -o $@ netclock_init.o netclock.o netclock_acsi.o -lgcc -s
	m68k-atari-mint-flags -q --mno-altram --mno-altalloc $@   # ACSI DMA: ST-RAM only (TT)

clean:
	rm -f NETTEST.TTP NETCLOCK.PRG *.o
