#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cam_v4l2.h"
#include "mpp_enc.h"
#include "udp_tx.h"

#define WIDTH  1920
#define HEIGHT 1536
#define FPS    30
#define BPS    (50 * 1000 * 1000)

static volatile int g_run = 1;

static void on_sig(int sig)
{
    (void)sig;
    g_run = 0;
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
        if (mpp_enc_encode(enc, mbuf, &pkt, &pkt_len, &key) == 0 && pkt_len > 0) {
            if (udp_tx_send_au(tx, pkt, pkt_len, frame_id, pts_ns, key, w->cam_id) != 0)
                fprintf(stderr, "cam%d send frame %u failed\n", w->cam_id, frame_id);
            if ((frame_id % 30) == 0)
                printf("cam%d frame %u seq=%u pts_ns=%llu len=%zu\n",
                       w->cam_id, frame_id, seq,
                       (unsigned long long)pts_ns, pkt_len);
            frame_id++;
        } else {
            fprintf(stderr, "cam%d encode failed\n", w->cam_id);
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
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ip> <port>\n", argv[0]);
        fprintf(stderr, "  cam0=/dev/video0 and cam1=/dev/video1 -> same ip:port\n");
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

    struct CamWorker w0 = { 0, "/dev/video0", ip, port };
    struct CamWorker w1 = { 1, "/dev/video1", ip, port };

    pthread_t th0, th1;
    if (pthread_create(&th0, NULL, cam_thread, &w0) != 0) {
        perror("pthread_create cam0");
        return 1;
    }
    if (pthread_create(&th1, NULL, cam_thread, &w1) != 0) {
        perror("pthread_create cam1");
        g_run = 0;
        pthread_join(th0, NULL);
        return 1;
    }

    pthread_join(th0, NULL);
    pthread_join(th1, NULL);
    return 0;
}
