#ifndef UDP_RTSTREAM_RECEIVER_CORE_H
#define UDP_RTSTREAM_RECEIVER_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ReceiverCore ReceiverCore;

typedef struct {
    int cam_id;
    uint32_t frame_id;
    uint64_t pts_ns;
    uint64_t enc_done_ns;
    uint64_t last_frag_tx_ns;
    uint64_t first_rx_ns;
    uint64_t recv_done_ns;
    int keyframe;
    const uint8_t *data;
    size_t size;
} ReceiverFrame;

typedef void (*ReceiverFrameCallback)(const ReceiverFrame *frame, void *user);

ReceiverCore *receiver_core_create(int udp_port, int camera_count);
void receiver_core_destroy(ReceiverCore *receiver);

/* 最多等待 timeout_ms；收到完整 H.264 AU 时同步调用 callback。 */
int receiver_core_poll(ReceiverCore *receiver, int timeout_ms,
                       ReceiverFrameCallback callback, void *user);

#ifdef __cplusplus
}
#endif

#endif
