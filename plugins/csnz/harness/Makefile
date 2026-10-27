# Build the CSNZ mp.dll execution harness (i686 Win32 PE, runs under Wine).
CC      ?= i686-w64-mingw32-gcc
CFLAGS  ?= -O2 -Wall
TARGET  := csnz_mp_harness.exe

$(TARGET): csnz_mp_harness.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: clean
