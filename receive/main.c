#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"
#ifdef USE_NVDEC
#include <SDL.h>
#include "nvdec_dec.h"
typedef NvDecCtx VideoDecCtx;
#define video_dec_create  nvdec_dec_create
#define video_dec_destroy nvdec_dec_destroy
#define video_dec_decode  nvdec_dec_decode
#define DECODER_NAME      "NVIDIA NVDEC"
#else
#include "mpp_dec.h"
typedef MppDecCtx VideoDecCtx;
#define video_dec_create  mpp_dec_create
#define video_dec_destroy mpp_dec_destroy
#define video_dec_decode  mpp_dec_decode
#define DECODER_NAME      "Rockchip MPP"
#endif

#define MAX_AU (8 * 1024 * 1024)
#define MAX_CAM 2

static volatile int g_run = 1;

static void on_sig(int sig)
{
    (void)sig;
    g_run = 0;
}

static uint16_t get_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static uint64_t now_realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct CamRx {
    VideoDecCtx *dec;
    uint8_t *au;
    uint32_t cur_frame;
    uint16_t expect_frag;
    uint16_t frag_cnt;
    size_t au_len;
    uint64_t pts_ns;
    uint64_t enc_done_ns;
    uint64_t first_rx_ns;
    int key;
    uint64_t last_pts;
    uint32_t frames;
};

int main(int argc, char **argv)
{
    /* ==================== 阶段 1：解析配置并初始化资源 ==================== */
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "bad port\n");
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    struct CamRx cams[MAX_CAM];
    memset(cams, 0, sizeof(cams));
    for (int i = 0; i < MAX_CAM; i++) {
        cams[i].dec = video_dec_create();
        cams[i].au = malloc(MAX_AU);
        cams[i].cur_frame = 0xffffffffu;
        if (!cams[i].dec || !cams[i].au) {
            fprintf(stderr, "cam%d init failed\n", i);
            return 1;
        }
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    int buf = 32 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef USE_NVDEC
    struct timeval rcv_to = {.tv_sec = 0, .tv_usec = 50000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    printf("listening udp port %d (2 cams, %s decode + delay)\n",
           port, DECODER_NAME);
    fflush(stdout);

#ifdef USE_NVDEC
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("udp_rtstream realtime",
                                       SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED,
                                       1920, 768, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1,
                                                 SDL_RENDERER_ACCELERATED) : NULL;
    SDL_Texture *tex[MAX_CAM] = {0};
    int show_w = 960, show_h = 768;
    if (!win || !ren) {
        fprintf(stderr, "SDL window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    for (int i = 0; i < MAX_CAM; i++) {
        tex[i] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   show_w, show_h);
        if (!tex[i]) {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            return 1;
        }
    }
    printf("realtime show: cam0|cam1 side-by-side %dx%d each\n", show_w, show_h);
    fflush(stdout);
#endif

    /* ==================== 阶段 2：接收 UDP 包并重组完整 AU ==================== */
    uint8_t pkt[URTS_PKT_MAX];
    while (g_run) {
#ifdef USE_NVDEC
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                g_run = 0;
        }
        if (!g_run)
            break;
#endif
        ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            perror("recvfrom");
            break;
        }
        if (n < URTS_HEADER_SIZE)
            continue;
        if (pkt[0] != URTS_MAGIC0 || pkt[1] != URTS_MAGIC1 ||
            pkt[2] != URTS_MAGIC2 || pkt[3] != URTS_MAGIC3)
            continue;
        if (pkt[4] != URTS_VERSION)
            continue;

        uint8_t flags = pkt[5];
        int cam_id = (int)URTS_CAM_ID(flags);
        if (cam_id < 0 || cam_id >= MAX_CAM)
            continue;

        struct CamRx *c = &cams[cam_id];
        uint32_t frame_id = get_be32(pkt + 10);
        uint64_t pts = get_be64(pkt + 14);
        uint16_t frag_idx = get_be16(pkt + 22);
        uint16_t fcnt = get_be16(pkt + 24);
        uint16_t plen = get_be16(pkt + 26);
        uint64_t enc_done = get_be64(pkt + 28);
        uint64_t frag_tx = get_be64(pkt + 36);
        uint64_t t_pkt = now_realtime_ns();

        if ((size_t)n < (size_t)URTS_HEADER_SIZE + (size_t)plen)
            continue;

        if (frame_id != c->cur_frame) {
            c->cur_frame = frame_id;
            c->expect_frag = 0;
            c->frag_cnt = fcnt;
            c->au_len = 0;
            c->pts_ns = pts;
            c->enc_done_ns = enc_done;
            c->first_rx_ns = t_pkt;
            c->key = (flags & URTS_FLAG_KEY) ? 1 : 0;
        }

        if (frag_idx != c->expect_frag || fcnt != c->frag_cnt) {
            c->expect_frag = 0;
            c->au_len = 0;
            if (frag_idx != 0)
                continue;
            c->cur_frame = frame_id;
            c->frag_cnt = fcnt;
            c->pts_ns = pts;
            c->enc_done_ns = enc_done;
            c->first_rx_ns = t_pkt;
            c->key = (flags & URTS_FLAG_KEY) ? 1 : 0;
        }

        if (c->au_len + plen > MAX_AU) {
            c->expect_frag = 0;
            c->au_len = 0;
            continue;
        }

        if (frag_idx == 0)
            c->first_rx_ns = t_pkt;

        memcpy(c->au + c->au_len, pkt + URTS_HEADER_SIZE, plen);
        c->au_len += plen;
        c->expect_frag++;

        if (flags & URTS_FLAG_LAST) {
            /* ==================== 阶段 3：硬解并统计端到端延迟 ==================== */
            uint64_t t_recv = now_realtime_ns();
            int64_t recv_delay_us = (int64_t)(t_recv - c->pts_ns) / 1000;
            int64_t enc_us = (int64_t)(c->enc_done_ns - c->pts_ns) / 1000;
            int64_t send_us = (int64_t)(frag_tx - c->enc_done_ns) / 1000;
            int64_t net_us = (int64_t)(t_recv - frag_tx) / 1000;
            int64_t reasm_us = 0;
            if (c->first_rx_ns)
                reasm_us = (int64_t)(t_recv - c->first_rx_ns) / 1000;

            int w = 0, h = 0;
            int dret = -1;
            int64_t decode_us = 0;
            int64_t show_us = 0;
            int64_t e2e_delay_us = recv_delay_us;
            if (c->au_len >= 128) {
#ifdef USE_NVDEC
                dret = video_dec_decode(c->dec, c->au, c->au_len, &w, &h);
                uint64_t t_dec1 = now_realtime_ns();
                nvdec_dec_last_timing(c->dec, &decode_us, &show_us);
#else
                uint64_t t_dec0 = now_realtime_ns();
                dret = video_dec_decode(c->dec, c->au, c->au_len, &w, &h);
                uint64_t t_dec1 = now_realtime_ns();
                decode_us = (int64_t)(t_dec1 - t_dec0) / 1000;
#endif
                e2e_delay_us = (int64_t)(t_dec1 - c->pts_ns) / 1000;
            }

            int64_t dt_us = 0;
            if (c->last_pts)
                dt_us = (int64_t)(c->pts_ns - c->last_pts) / 1000;
            c->last_pts = c->pts_ns;
            c->frames++;

            if ((c->frames % 30) == 1 || c->frames <= 3 || dret != 0) {
#ifdef USE_NVDEC
                printf("cam%d frame %u pts_ns=%llu au=%zu %dx%d "
                       "pts_dt_us=%lld recv_delay_us=%lld decode_us=%lld show_us=%lld e2e_delay_us=%lld ok=%d\n",
                       cam_id,
                       frame_id,
                       (unsigned long long)c->pts_ns,
                       c->au_len,
                       w, h,
                       (long long)dt_us,
                       (long long)recv_delay_us,
                       (long long)decode_us,
                       (long long)show_us,
                       (long long)e2e_delay_us,
                       dret == 0);
#else
                printf("cam%d frame %u pts_ns=%llu au=%zu %dx%d "
                       "pts_dt_us=%lld enc_us=%lld send_us=%lld net_us=%lld "
                       "reasm_us=%lld recv_delay_us=%lld decode_us=%lld "
                       "e2e_delay_us=%lld ok=%d\n",
                       cam_id,
                       frame_id,
                       (unsigned long long)c->pts_ns,
                       c->au_len,
                       w, h,
                       (long long)dt_us,
                       (long long)enc_us,
                       (long long)send_us,
                       (long long)net_us,
                       (long long)reasm_us,
                       (long long)recv_delay_us,
                       (long long)decode_us,
                       (long long)e2e_delay_us,
                       dret == 0);
#endif
                fflush(stdout);
            }

#ifdef USE_NVDEC
            if (dret == 0) {
                int rw = 0, rh = 0;
                const uint8_t *rgb = nvdec_dec_rgb(c->dec, &rw, &rh);
                if (rgb && rw == show_w && rh == show_h) {
                    SDL_UpdateTexture(tex[cam_id], NULL, rgb, show_w * 3);
                    for (int i = 0; i < MAX_CAM; i++) {
                        SDL_Rect dst = {i * show_w, 0, show_w, show_h};
                        SDL_RenderCopy(ren, tex[i], NULL, &dst);
                    }
                    SDL_RenderPresent(ren);
                }
            }
#endif
            c->expect_frag = 0;
            c->au_len = 0;
        }
    }

    /* ==================== 阶段 4：释放接收与解码资源 ==================== */
#ifdef USE_NVDEC
    for (int i = 0; i < MAX_CAM; i++)
        SDL_DestroyTexture(tex[i]);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
#endif
    for (int i = 0; i < MAX_CAM; i++) {
        free(cams[i].au);
        video_dec_destroy(cams[i].dec);
    }
    close(fd);
    return 0;
}
