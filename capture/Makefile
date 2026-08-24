CC = gcc
include ../config.env

.DEFAULT_GOAL := all

CONFIG_CFLAGS = -DWIDTH=$(VIDEO_WIDTH) -DHEIGHT=$(VIDEO_HEIGHT) -DFPS=$(VIDEO_FPS) -DBPS=$(VIDEO_BITRATE) -DNUM_CAMS=$(CAMERA_COUNT) -DENCODER_GOP=$(VIDEO_GOP)
CFLAGS = -O2 -Wall -Wextra -I/usr/include/rockchip -I/usr/local/include/rockchip -I. $(CONFIG_CFLAGS)
LDFLAGS = -L/usr/local/lib -L/usr/lib/aarch64-linux-gnu -lrockchip_mpp -lpthread
OBJS = main.o cam_v4l2.o mpp_enc.o udp_tx.o

$(OBJS): ../config.env

.PHONY: all clean

all: udp_rtstream

udp_rtstream: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) udp_rtstream
