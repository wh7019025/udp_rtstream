CC = gcc
CFLAGS = -O2 -Wall -Wextra -Werror $(shell pkg-config --cflags sdl2)
LDFLAGS = $(shell pkg-config --libs sdl2)

.PHONY: all run clean

all: time_sync_test

time_sync_test: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: time_sync_test
	./time_sync_test

clean:
	rm -f time_sync_test
