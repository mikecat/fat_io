CC=gcc
CFLAGS=-O2 -Wall -Wextra -s -static

fat_io.exe: fat_io.c
	$(CC) $(CFLAGS) -o fat_io.exe fat_io.c
