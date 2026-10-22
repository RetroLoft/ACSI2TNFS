# STINGINS.TOS: STinG installer for ACSI2TNFS (m68k-atari-mint-gcc, e.g. in WSL)
CC = m68k-atari-mint-gcc
CFLAGS = -m68000 -mshort -O2 -Wall -fomit-frame-pointer -fno-builtin -I../sting/include
STING = ../sting

STINGINS.TOS: init.o stingins.o acsinet.o payload.o
	$(CC) -mshort -nostdlib -o $@ $^ -lgcc -s
	m68k-atari-mint-flags -q --mno-altram --mno-altalloc $@   # ACSI DMA: ST-RAM only (TT)

init.o: $(STING)/init.S
	$(CC) $(CFLAGS) -c $< -o $@

acsinet.o: $(STING)/acsinet.c $(STING)/acsinet.h
	$(CC) $(CFLAGS) -c $< -o $@

stingins.o: stingins.c $(STING)/acsinet.h
	$(CC) $(CFLAGS) -c $< -o $@

payload.c: mkpayload.py $(wildcard sting/*) $(STING)/ACSI_NET.STX
	python3 mkpayload.py $@

payload.o: payload.c
	$(CC) $(CFLAGS) -c $< -o $@

$(STING)/ACSI_NET.STX:
	$(MAKE) -C $(STING)

clean:
	rm -f STINGINS.TOS *.o payload.c
