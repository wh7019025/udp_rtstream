CC = gcc
CFLAGS = -O2 -Wall -Wextra -I/usr/include/rockchip -I/usr/local/include/rockchip -I.
LDFLAGS = -L/usr/local/lib -L/usr/lib/aarch64-linux-gnu -lrockchip_mpp -lpthread

OBJS = main.o mpp_dec.o

.PHONY: all clean

all: udp_rtstream_rx

udp_rtstream_rx: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) udp_rtstream_rx
