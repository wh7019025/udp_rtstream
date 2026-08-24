#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <SDL.h>

#define LOGICAL_W 1920
#define LOGICAL_H 1080
#define TEST_FPS 60
#define GRID_COLS 10
#define GRID_ROWS 6

static const uint8_t DIGIT_SEGMENTS[10] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

static void set_color(SDL_Renderer *r, uint8_t red, uint8_t green, uint8_t blue)
{
    SDL_SetRenderDrawColor(r, red, green, blue, 255);
}

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      uint8_t red, uint8_t green, uint8_t blue)
{
    SDL_Rect rect = {x, y, w, h};
    set_color(r, red, green, blue);
    SDL_RenderFillRect(r, &rect);
}

static void draw_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      uint8_t red, uint8_t green, uint8_t blue)
{
    SDL_Rect rect = {x, y, w, h};
    set_color(r, red, green, blue);
    SDL_RenderDrawRect(r, &rect);
}

static void draw_digit(SDL_Renderer *r, int digit, int x, int y,
                       int w, int h, int t,
                       uint8_t red, uint8_t green, uint8_t blue)
{
    if (digit < 0 || digit > 9)
        return;

    uint8_t s = DIGIT_SEGMENTS[digit];
    int vh = (h - 3 * t) / 2;
    int lower_y = y + 2 * t + vh;

    if (s & 0x01) fill_rect(r, x + t, y, w - 2 * t, t, red, green, blue);
    if (s & 0x02) fill_rect(r, x + w - t, y + t, t, vh, red, green, blue);
    if (s & 0x04) fill_rect(r, x + w - t, lower_y, t, vh, red, green, blue);
    if (s & 0x08) fill_rect(r, x + t, y + h - t, w - 2 * t, t, red, green, blue);
    if (s & 0x10) fill_rect(r, x, lower_y, t, vh, red, green, blue);
    if (s & 0x20) fill_rect(r, x, y + t, t, vh, red, green, blue);
    if (s & 0x40) fill_rect(r, x + t, y + t + vh, w - 2 * t, t, red, green, blue);
}

static void draw_two_digits(SDL_Renderer *r, int value, int x, int y,
                            int w, int h, int t,
                            uint8_t red, uint8_t green, uint8_t blue)
{
    int gap = 2 * t;
    int dw = (w - gap) / 2;
    draw_digit(r, value / 10, x, y, dw, h, t, red, green, blue);
    draw_digit(r, value % 10, x + dw + gap, y, dw, h, t, red, green, blue);
}

static void draw_timestamp(SDL_Renderer *r, uint64_t epoch_ms)
{
    /* 固定显示 10 位 Unix 秒和 3 位毫秒：SSSSSSSSSS.mmm。 */
    uint64_t seconds = epoch_ms / 1000;
    int milliseconds = (int)(epoch_ms % 1000);
    int x = 425;
    int y = 1005;
    int digit_w = 54;
    int digit_h = 66;
    int gap = 10;
    int thickness = 8;

    uint64_t divisor = 1000000000ULL;
    for (int index = 0; index < 10; index++) {
        int digit = (int)((seconds / divisor) % 10);
        draw_digit(r, digit, x, y, digit_w, digit_h, thickness,
                   90, 200, 255);
        x += digit_w + gap;
        divisor /= 10;
    }

    fill_rect(r, x, y + digit_h - thickness, thickness, thickness,
              90, 200, 255);
    x += gap + thickness;

    divisor = 100;
    for (int index = 0; index < 3; index++) {
        int digit = (milliseconds / (int)divisor) % 10;
        draw_digit(r, digit, x, y, digit_w, digit_h, thickness,
                   150, 155, 160);
        x += digit_w + gap;
        divisor /= 10;
    }
}

static void draw_phase_grid(SDL_Renderer *r, int phase)
{
    int grid_x = 90;
    int grid_y = 300;
    int cell_w = 174;
    int cell_h = 98;

    for (int index = 0; index < TEST_FPS; index++) {
        int x = grid_x + (index % GRID_COLS) * cell_w;
        int y = grid_y + (index / GRID_COLS) * cell_h;
        draw_rect(r, x, y, cell_w - 12, cell_h - 12, 105, 105, 105);
        if (index == phase)
            fill_rect(r, x + 7, y + 7, cell_w - 26, cell_h - 26,
                      105, 115, 125);
    }
}

static void draw_test_pattern(SDL_Renderer *r, const struct timespec *now)
{
    /* ==================== 阶段 1：从绝对时间计算 60 Hz 相位 ==================== */
    int phase = (int)(((uint64_t)now->tv_nsec * TEST_FPS) / 1000000000ULL);
    int second = (int)(now->tv_sec % 60);
    uint64_t frame_epoch_ms = (uint64_t)now->tv_sec * 1000ULL +
                              (uint64_t)phase * 1000ULL / TEST_FPS;

    /* ==================== 阶段 2：绘制稳定低亮度背景、秒数和帧号 ==================== */
    set_color(r, 8, 8, 8);
    SDL_RenderClear(r);
    fill_rect(r, 70, 55, 105, 105, 95, 45, 40);
    fill_rect(r, 205, 55, 105, 105, 105, 100, 40);
    draw_two_digits(r, second, 370, 40, 250, 145, 18, 90, 200, 255);
    draw_two_digits(r, phase, 1490, 35, 340, 190, 24, 185, 190, 195);

    /* ==================== 阶段 3：绘制稳定的 60 格帧位置 ==================== */
    draw_phase_grid(r, phase);
    draw_timestamp(r, frame_epoch_ms);

    /* 红线显示秒内相位，也便于观察滚动快门。 */
    int scan_x = (int)(((uint64_t)now->tv_nsec * LOGICAL_W) / 1000000000ULL);
    fill_rect(r, scan_x, 0, 3, LOGICAL_H, 115, 45, 45);
}

int main(void)
{
    /* ==================== 阶段 1：创建垂直同步全屏窗口 ==================== */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "60 Hz time synchronization test",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        LOGICAL_W, LOGICAL_H,
        SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;

    if (!window || !renderer) {
        fprintf(stderr, "SDL window/renderer failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(renderer, LOGICAL_W, LOGICAL_H);

    /* ==================== 阶段 2：持续生成绝对时间测试帧 ==================== */
    int running = 1;
    int fullscreen = 1;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN &&
                 (event.key.keysym.sym == SDLK_q ||
                  event.key.keysym.sym == SDLK_ESCAPE)))
                running = 0;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_f) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(window,
                    fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
        }

        struct timespec realtime;
        clock_gettime(CLOCK_REALTIME, &realtime);
        draw_test_pattern(renderer, &realtime);
        SDL_RenderPresent(renderer);
    }

    /* ==================== 阶段 3：释放显示资源 ==================== */
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
