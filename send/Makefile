CC = gcc
CFLAGS = -O2 -Wall -Wextra -I/usr/include/rockchip -I/usr/local/include/rockchip -I.
LDFLAGS = -L/usr/local/lib -L/usr/lib/aarch64-linux-gnu -lrockchip_mpp -lpthread

OBJS = main.o cam_v4l2.o mpp_enc.o udp_tx.o

.PHONY: all clean

all: udp_rtstream

udp_rtstream: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) udp_rtstream
