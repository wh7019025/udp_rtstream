CC = gcc
CFLAGS = -O2 -Wall -Wextra -I/usr/include/rockchip -I/usr/local/include/rockchip -I.
LDFLAGS = -L/usr/local/lib -L/usr/lib/aarch64-linux-gnu -lrockchip_mpp -lpthread

OBJS = main.o mpp_dec.o
NVDEC_OBJS = main.nvdec.o nvdec_dec.o
NVDEC_CFLAGS = $(shell pkg-config --cflags libavcodec libavutil libswscale sdl2)
NVDEC_LDFLAGS = $(shell pkg-config --libs libavcodec libavutil libswscale sdl2)

.PHONY: all nvidia clean

all: udp_rtstream_rx

nvidia: udp_rtstream_rx_nvidia

udp_rtstream_rx: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

udp_rtstream_rx_nvidia: $(NVDEC_OBJS)
	$(CC) -o $@ $(NVDEC_OBJS) $(NVDEC_LDFLAGS)

main.nvdec.o: main.c nvdec_dec.h proto.h
	$(CC) -O2 -Wall -Wextra -I. $(NVDEC_CFLAGS) -DUSE_NVDEC -c -o $@ $<

nvdec_dec.o: nvdec_dec.c nvdec_dec.h
	$(CC) -O2 -Wall -Wextra -I. $(NVDEC_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(NVDEC_OBJS) udp_rtstream_rx udp_rtstream_rx_nvidia
