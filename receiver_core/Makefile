CC = gcc
include ../config.env

CONFIG_CFLAGS = -DMAX_CAM=$(CAMERA_COUNT) -DDEFAULT_UDP_PORT=$(UDP_PORT) -DSTATS_WINDOW_FRAMES=$(STATS_WINDOW_FRAMES) -DDELAY_PROBE_HOST=\"$(REMOTE_HOST)\" -DDELAY_PROBE_PORT=$(DELAY_PROBE_PORT) -DDELAY_CHECK_THRESHOLD_US=$(DELAY_CHECK_THRESHOLD_US) -DDELAY_CHECK_SAMPLES=$(DELAY_CHECK_SAMPLES) -DDELAY_CHECK_INTERVAL_MS=$(DELAY_CHECK_INTERVAL_MS)
CFLAGS = -O2 -Wall -Wextra -I/usr/include/rockchip -I/usr/local/include/rockchip -I. $(CONFIG_CFLAGS)
MPP_LDFLAGS = -L/usr/local/lib -L/usr/lib/aarch64-linux-gnu -lrockchip_mpp -lpthread
NVDEC_CFLAGS = $(shell pkg-config --cflags libavcodec libavutil libswscale sdl2)
NVDEC_LDFLAGS = $(shell pkg-config --libs libavcodec libavutil libswscale sdl2)

CORE_OBJS = receiver_core.o delay_probe.o
MPP_OBJS = debug_main.o mpp_dec.o $(CORE_OBJS)
NVDEC_OBJS = debug_main.nvdec.o nvdec_dec.o $(CORE_OBJS)

$(MPP_OBJS) $(NVDEC_OBJS): ../config.env

.PHONY: all nvidia ros clean

# ==================== 阶段 1：构建无 ROS 调试程序 ====================
all: debug_receiver_mpp

nvidia: debug_receiver_nvidia

debug_receiver_mpp: $(MPP_OBJS)
	$(CC) -o $@ $(MPP_OBJS) $(MPP_LDFLAGS)

debug_receiver_nvidia: $(NVDEC_OBJS)
	$(CC) -o $@ $(NVDEC_OBJS) $(NVDEC_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

debug_main.nvdec.o: debug_main.c nvdec_dec.h receiver_core.h
	$(CC) $(CFLAGS) $(NVDEC_CFLAGS) -DUSE_NVDEC -c -o $@ $<

nvdec_dec.o: nvdec_dec.c nvdec_dec.h
	$(CC) -O2 -Wall -Wextra -I. $(NVDEC_CFLAGS) -c -o $@ $<

# ==================== 阶段 2：构建 ROS 2 wrapper ====================
ros:
	cd ../ros_wrapper && /usr/bin/colcon build --symlink-install \
		--cmake-clean-cache --cmake-args -DPython3_EXECUTABLE=/usr/bin/python3

# ==================== 阶段 3：清理本目录构建产物 ====================
clean:
	rm -f $(MPP_OBJS) $(NVDEC_OBJS) debug_receiver_mpp debug_receiver_nvidia
	rm -rf ../ros_wrapper/build ../ros_wrapper/install ../ros_wrapper/log
