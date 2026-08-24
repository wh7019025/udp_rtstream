#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "receiver_core.h"

#ifdef USE_NVDEC
#include <SDL.h>
#include "nvdec_dec.h"
typedef NvDecCtx VideoDecCtx;
#define video_dec_create nvdec_dec_create
#define video_dec_destroy nvdec_dec_destroy
#define video_dec_decode nvdec_dec_decode
#define DECODER_NAME "NVIDIA NVDEC"
#else
#include "mpp_dec.h"
typedef MppDecCtx VideoDecCtx;
#define video_dec_create mpp_dec_create
#define video_dec_destroy mpp_dec_destroy
#define video_dec_decode mpp_dec_decode
#define DECODER_NAME "Rockchip MPP"
#endif

#ifndef MAX_CAM
#error "CAMERA_COUNT must be supplied by config.env through Makefile"
#endif
#ifndef DEFAULT_UDP_PORT
#error "UDP_PORT must be supplied by config.env through Makefile"
#endif
#ifndef STATS_WINDOW_FRAMES
#error "STATS_WINDOW_FRAMES must be supplied by config.env through Makefile"
#endif

#define PREVIEW_COLS 2
#define PREVIEW_W 640
#define PREVIEW_H 512

enum DelayMetric {
    DELAY_ENC, DELAY_SEND, DELAY_NET, DELAY_REASM,
    DELAY_RECV, DELAY_DECODE, DELAY_SHOW, DELAY_E2E,
    DELAY_METRIC_COUNT
};

struct DelayRangeStats {
    uint32_t count;
    int64_t min_us[DELAY_METRIC_COUNT];
    int64_t max_us[DELAY_METRIC_COUNT];
};

struct DebugCamera {
    VideoDecCtx *decoder;
    uint64_t display_pts_ns;
    uint64_t last_pts_ns;
    struct DelayRangeStats stats;
};

struct DebugApp {
    struct DebugCamera cameras[MAX_CAM];
#ifdef USE_NVDEC
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *textures[MAX_CAM];
#endif
};

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static uint64_t realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void stats_add(struct DelayRangeStats *stats,
                      const int64_t values[DELAY_METRIC_COUNT])
{
    for (int i = 0; i < DELAY_METRIC_COUNT; i++) {
        if (stats->count == 0 || values[i] < stats->min_us[i])
            stats->min_us[i] = values[i];
        if (stats->count == 0 || values[i] > stats->max_us[i])
            stats->max_us[i] = values[i];
    }
    stats->count++;
}

static void stats_print_and_reset(int cam_id, struct DelayRangeStats *stats)
{
    static const char *names[DELAY_METRIC_COUNT] = {
        "enc", "send", "net", "reasm", "recv", "decode", "show", "e2e"
    };
    printf("cam%d latency_range_us samples=%u", cam_id, stats->count);
    for (int i = 0; i < DELAY_METRIC_COUNT; i++)
        printf(" %s=%lld..%lld", names[i],
               (long long)stats->min_us[i], (long long)stats->max_us[i]);
    printf("\n");
    fflush(stdout);
    memset(stats, 0, sizeof(*stats));
}

#ifdef USE_NVDEC
static const uint8_t DIGIT_SEGMENTS[10] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

static void fill_rect(SDL_Renderer *renderer, int x, int y, int w, int h,
                      uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    SDL_Rect rect = {x, y, w, h};
    SDL_SetRenderDrawColor(renderer, red, green, blue, alpha);
    SDL_RenderFillRect(renderer, &rect);
}

static void draw_digit(SDL_Renderer *renderer, int digit, int x, int y)
{
    const int width = 28, height = 48, thickness = 5;
    const int vertical = (height - 3 * thickness) / 2;
    const int lower_y = y + 2 * thickness + vertical;
    uint8_t segments = DIGIT_SEGMENTS[digit];
    if (segments & 0x01) fill_rect(renderer, x + thickness, y, width - 2 * thickness, thickness, 255, 220, 80, 255);
    if (segments & 0x02) fill_rect(renderer, x + width - thickness, y + thickness, thickness, vertical, 255, 220, 80, 255);
    if (segments & 0x04) fill_rect(renderer, x + width - thickness, lower_y, thickness, vertical, 255, 220, 80, 255);
    if (segments & 0x08) fill_rect(renderer, x + thickness, y + height - thickness, width - 2 * thickness, thickness, 255, 220, 80, 255);
    if (segments & 0x10) fill_rect(renderer, x, lower_y, thickness, vertical, 255, 220, 80, 255);
    if (segments & 0x20) fill_rect(renderer, x, y + thickness, thickness, vertical, 255, 220, 80, 255);
    if (segments & 0x40) fill_rect(renderer, x + thickness, y + thickness + vertical, width - 2 * thickness, thickness, 255, 220, 80, 255);
}

static void draw_timestamp(SDL_Renderer *renderer, uint64_t pts_ns, int x, int y)
{
    if (!pts_ns)
        return;
    uint64_t seconds = pts_ns / 1000000000ULL;
    int milliseconds = (int)((pts_ns / 1000000ULL) % 1000ULL);
    int cursor = x + 10;
    uint64_t divisor = 1000000000ULL;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    fill_rect(renderer, x, y, 458, 68, 0, 0, 0, 190);
    for (int i = 0; i < 10; i++) {
        draw_digit(renderer, (int)((seconds / divisor) % 10), cursor, y + 10);
        cursor += 33;
        divisor /= 10;
    }
    fill_rect(renderer, cursor, y + 52, 6, 6, 255, 220, 80, 255);
    cursor += 16;
    for (divisor = 100; divisor > 0; divisor /= 10) {
        draw_digit(renderer, (milliseconds / (int)divisor) % 10, cursor, y + 10);
        cursor += 33;
    }
}

static void render_all(struct DebugApp *app)
{
    for (int i = 0; i < MAX_CAM; i++) {
        SDL_Rect destination = {
            (i % PREVIEW_COLS) * PREVIEW_W,
            (i / PREVIEW_COLS) * PREVIEW_H,
            PREVIEW_W, PREVIEW_H
        };
        SDL_RenderCopy(app->renderer, app->textures[i], NULL, &destination);
        draw_timestamp(app->renderer, app->cameras[i].display_pts_ns,
                       destination.x + 12, destination.y + 12);
    }
    SDL_RenderPresent(app->renderer);
}
#endif

static void handle_frame(const ReceiverFrame *frame, void *user)
{
    /* ==================== 阶段 3：解码并计算流水线延迟 ==================== */
    struct DebugApp *app = user;
    struct DebugCamera *camera = &app->cameras[frame->cam_id];
    int width = 0, height = 0;
    uint64_t decode_start_ns = realtime_ns();
    int decoded = video_dec_decode(camera->decoder, frame->data, frame->size,
                                   &width, &height);
    uint64_t decode_done_ns = realtime_ns();
    int64_t decode_us = (int64_t)(decode_done_ns - decode_start_ns) / 1000;
    int64_t show_us = 0;
#ifdef USE_NVDEC
    nvdec_dec_last_timing(camera->decoder, &decode_us, &show_us);
#endif

    int64_t pts_dt_us = camera->last_pts_ns
        ? (int64_t)(frame->pts_ns - camera->last_pts_ns) / 1000 : 0;
    camera->last_pts_ns = frame->pts_ns;
    int64_t values[DELAY_METRIC_COUNT] = {
        (int64_t)(frame->enc_done_ns - frame->pts_ns) / 1000,
        (int64_t)(frame->last_frag_tx_ns - frame->enc_done_ns) / 1000,
        (int64_t)(frame->recv_done_ns - frame->last_frag_tx_ns) / 1000,
        (int64_t)(frame->recv_done_ns - frame->first_rx_ns) / 1000,
        (int64_t)(frame->recv_done_ns - frame->pts_ns) / 1000,
        decode_us,
        show_us,
        (int64_t)(decode_done_ns - frame->pts_ns) / 1000,
    };
    stats_add(&camera->stats, values);
    if (camera->stats.count >= STATS_WINDOW_FRAMES)
        stats_print_and_reset(frame->cam_id, &camera->stats);

    if (decoded != 0) {
        fprintf(stderr, "cam%d frame %u decode failed pts_dt_us=%lld\n",
                frame->cam_id, frame->frame_id, (long long)pts_dt_us);
        return;
    }

#ifdef USE_NVDEC
    /* ==================== 阶段 4：刷新无 ROS 调试预览 ==================== */
    int rgb_width = 0, rgb_height = 0;
    const uint8_t *rgb = nvdec_dec_rgb(camera->decoder,
                                       &rgb_width, &rgb_height);
    if (rgb && rgb_width == PREVIEW_W && rgb_height == PREVIEW_H) {
        camera->display_pts_ns = frame->pts_ns;
        SDL_UpdateTexture(app->textures[frame->cam_id], NULL,
                          rgb, PREVIEW_W * 3);
        render_all(app);
    }
#endif
}

int main(void)
{
    /* ==================== 阶段 1：初始化共享接收底层与解码器 ==================== */
    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);
    ReceiverCore *receiver = receiver_core_create(DEFAULT_UDP_PORT, MAX_CAM);
    struct DebugApp app;
    memset(&app, 0, sizeof(app));
    if (!receiver) {
        fprintf(stderr, "receiver core init failed\n");
        return 1;
    }
    for (int i = 0; i < MAX_CAM; i++) {
        app.cameras[i].decoder = video_dec_create();
        if (!app.cameras[i].decoder) {
            fprintf(stderr, "cam%d decoder init failed\n", i);
            running = 0;
        }
    }

#ifdef USE_NVDEC
    /* ==================== 阶段 2：初始化 SDL 调试界面 ==================== */
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        running = 0;
    app.window = SDL_CreateWindow("udp_rtstream debug receiver",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        PREVIEW_COLS * PREVIEW_W,
        ((MAX_CAM + PREVIEW_COLS - 1) / PREVIEW_COLS) * PREVIEW_H, 0);
    app.renderer = app.window
        ? SDL_CreateRenderer(app.window, -1, SDL_RENDERER_ACCELERATED) : NULL;
    for (int i = 0; i < MAX_CAM && app.renderer; i++)
        app.textures[i] = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_RGB24,
            SDL_TEXTUREACCESS_STREAMING, PREVIEW_W, PREVIEW_H);
    if (!app.window || !app.renderer)
        running = 0;
#endif

    printf("debug receiver: UDP %d, %d cams, %s; press q to quit\n",
           DEFAULT_UDP_PORT, MAX_CAM, DECODER_NAME);
    fflush(stdout);

    /* ==================== 阶段 5：轮询共享底层并处理退出事件 ==================== */
    while (running) {
#ifdef USE_NVDEC
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_q))
                running = 0;
        }
#endif
        if (running && receiver_core_poll(receiver, 50, handle_frame, &app) < 0) {
            perror("receiver poll");
            break;
        }
    }

    /* ==================== 阶段 6：统一释放资源 ==================== */
#ifdef USE_NVDEC
    for (int i = 0; i < MAX_CAM; i++)
        SDL_DestroyTexture(app.textures[i]);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
#endif
    for (int i = 0; i < MAX_CAM; i++)
        video_dec_destroy(app.cameras[i].decoder);
    receiver_core_destroy(receiver);
    return 0;
}
