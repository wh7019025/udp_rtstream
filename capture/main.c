#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cam_v4l2.h"
#include "mpp_enc.h"
#include "udp_tx.h"

#if !defined(WIDTH) || !defined(HEIGHT) || !defined(FPS) || \
    !defined(BPS) || !defined(NUM_CAMS)
#error "video config must be supplied by config.env through Makefile"
#endif

static volatile int g_run = 1;

static void on_sig(int sig)
{
    (void)sig;
    g_run = 0;
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static uint64_t realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct CamWorker {
    int cam_id;
    const char *device;
    const char *ip;
    int port;
};

static void *cam_thread(void *arg)
{
    struct CamWorker *w = arg;

    CamV4l2 *cam = cam_v4l2_open(w->device, WIDTH, HEIGHT, FPS, 4);
    if (!cam) {
        fprintf(stderr, "cam%d open %s failed\n", w->cam_id, w->device);
        return NULL;
    }

    MppEncCtx *enc = mpp_enc_create(WIDTH, HEIGHT, FPS, BPS);
    if (!enc) {
        fprintf(stderr, "cam%d encoder create failed\n", w->cam_id);
        cam_v4l2_close(cam);
        return NULL;
    }

    UdpTx *tx = udp_tx_open(w->ip, w->port);
    if (!tx) {
        fprintf(stderr, "cam%d udp open failed\n", w->cam_id);
        mpp_enc_destroy(enc);
        cam_v4l2_close(cam);
        return NULL;
    }

    for (int i = 0; i < 10 && g_run; i++) {
        uint64_t pts;
        uint32_t seq;
        int idx = cam_v4l2_get_frame(cam, &pts, &seq);
        if (idx >= 0)
            cam_v4l2_put_frame(cam, idx);
        else {
            fprintf(stderr, "cam%d warmup got no frame, continue anyway\n", w->cam_id);
            break;
        }
    }

    printf("cam%d streaming %s %dx%d -> udp://%s:%d\n",
           w->cam_id, w->device, WIDTH, HEIGHT, w->ip, w->port);
    fflush(stdout);

    uint32_t frame_id = 0;
    int encode_samples = 0;
    int64_t encode_sum_us = 0;
    int64_t encode_min_us = LLONG_MAX;
    int64_t encode_max_us = LLONG_MIN;
    int bitrate_samples = 0;
    uint64_t encoded_bytes = 0;
    int64_t bitrate_start_ns = mono_ns();
    while (g_run) {
        uint64_t pts_ns = 0;
        uint32_t seq = 0;
        int idx = cam_v4l2_get_frame(cam, &pts_ns, &seq);
        if (idx < 0) {
            fprintf(stderr, "cam%d get_frame failed, retry\n", w->cam_id);
            continue;
        }

        MppBuffer mbuf = cam_v4l2_buf(cam, idx);
        const uint8_t *pkt = NULL;
        size_t pkt_len = 0;
        int key = 0;
        int64_t t0 = mono_ns();
        int enc_ok = mpp_enc_encode(enc, mbuf, &pkt, &pkt_len, &key);
        uint64_t enc_done_ns = realtime_ns();
        int64_t encode_us = (mono_ns() - t0) / 1000;
        encode_samples++;
        encode_sum_us += encode_us;
        if (encode_us < encode_min_us)
            encode_min_us = encode_us;
        if (encode_us > encode_max_us)
            encode_max_us = encode_us;
        if (encode_samples == 300) {
            printf("cam%d encode_stats_us samples=%d avg=%lld min=%lld max=%lld\n",
                   w->cam_id, encode_samples,
                   (long long)(encode_sum_us / encode_samples),
                   (long long)encode_min_us, (long long)encode_max_us);
            fflush(stdout);
            encode_samples = 0;
            encode_sum_us = 0;
            encode_min_us = LLONG_MAX;
            encode_max_us = LLONG_MIN;
        }

        if (enc_ok == 0 && pkt_len > 0) {
            bitrate_samples++;
            encoded_bytes += pkt_len;
            if (bitrate_samples == 300) {
                int64_t elapsed_ns = mono_ns() - bitrate_start_ns;
                uint64_t actual_bps = elapsed_ns > 0
                    ? encoded_bytes * 8ULL * 1000000000ULL /
                      (uint64_t)elapsed_ns
                    : 0;
                printf("cam%d bitrate_stats frames=%d avg_au_bytes=%llu "
                       "actual_mbps=%.2f target_mbps=%.2f\n",
                       w->cam_id, bitrate_samples,
                       (unsigned long long)(encoded_bytes /
                                            (uint64_t)bitrate_samples),
                       (double)actual_bps / 1000000.0,
                       (double)BPS / 1000000.0);
                fflush(stdout);
                bitrate_samples = 0;
                encoded_bytes = 0;
                bitrate_start_ns = mono_ns();
            }
            if (udp_tx_send_au(tx, pkt, pkt_len, frame_id, pts_ns, enc_done_ns,
                               key, w->cam_id) != 0)
                fprintf(stderr, "cam%d send frame %u failed\n", w->cam_id, frame_id);
            if ((frame_id % 30) == 0) {
                printf("cam%d frame %u seq=%u pts_ns=%llu len=%zu encode_us=%lld key=%d\n",
                       w->cam_id, frame_id, seq,
                       (unsigned long long)pts_ns, pkt_len,
                       (long long)encode_us, key);
                fflush(stdout);
            }
            frame_id++;
        } else {
            fprintf(stderr, "cam%d encode failed encode_us=%lld\n",
                    w->cam_id, (long long)encode_us);
        }

        cam_v4l2_put_frame(cam, idx);
    }

    udp_tx_close(tx);
    mpp_enc_destroy(enc);
    cam_v4l2_close(cam);
    return NULL;
}

int main(int argc, char **argv)
{
    /* ==================== 阶段 1：解析发送目标 ==================== */
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ip> <port>\n", argv[0]);
        fprintf(stderr, "  cam0..cam3=/dev/video0../dev/video3 -> same ip:port\n");
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "bad port\n");
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* ==================== 阶段 2：启动四路采集、编码与发送线程 ==================== */
    const char *devices[] = {
        "/dev/video0", "/dev/video1", "/dev/video2", "/dev/video3"
    };
    struct CamWorker workers[NUM_CAMS];
    pthread_t threads[NUM_CAMS];
    int started = 0;

    for (int i = 0; i < NUM_CAMS; i++) {
        workers[i] = (struct CamWorker){i, devices[i], ip, port};
        if (pthread_create(&threads[i], NULL, cam_thread, &workers[i]) != 0) {
            fprintf(stderr, "pthread_create cam%d failed\n", i);
            g_run = 0;
            break;
        }
        started++;
    }

    /* ==================== 阶段 3：等待线程退出并统一回收 ==================== */
    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);

    return started == NUM_CAMS ? 0 : 1;
}
