#include "nvdec_dec.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <time.h>

#define SHOW_W 960
#define SHOW_H 768

struct NvDecCtx {
    AVCodecContext *codec_ctx;
    AVBufferRef *hw_device_ctx;
    enum AVPixelFormat hw_pix_fmt;
    struct SwsContext *sws;
    enum AVPixelFormat sws_src_fmt;
    int sws_src_w;
    int sws_src_h;
    uint8_t *rgb;
    int rgb_ready;
    int64_t last_hw_us;
    int64_t last_show_us;
};

static void print_av_error(const char *operation, int err)
{
    char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, text, sizeof(text));
    fprintf(stderr, "%s failed: %s\n", operation, text);
}

static enum AVPixelFormat select_hw_format(AVCodecContext *codec_ctx,
                                            const enum AVPixelFormat *formats)
{
    NvDecCtx *dec = codec_ctx->opaque;

    for (const enum AVPixelFormat *fmt = formats;
         *fmt != AV_PIX_FMT_NONE; fmt++) {
        if (*fmt == dec->hw_pix_fmt)
            return *fmt;
    }

    fprintf(stderr, "NVDEC did not offer the required CUDA pixel format\n");
    return AV_PIX_FMT_NONE;
}

NvDecCtx *nvdec_dec_create(void)
{
    /* ==================== 阶段 1：定位支持 CUDA 的 H.264 解码器 ==================== */
    NvDecCtx *dec = calloc(1, sizeof(*dec));
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!dec || !codec)
        goto fail;

    const AVCodecHWConfig *config = NULL;
    for (int i = 0; (config = avcodec_get_hw_config(codec, i)); i++) {
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            config->device_type == AV_HWDEVICE_TYPE_CUDA) {
            dec->hw_pix_fmt = config->pix_fmt;
            break;
        }
    }
    if (!config) {
        fprintf(stderr, "FFmpeg H.264 decoder has no NVIDIA CUDA/NVDEC support\n");
        goto fail;
    }

    /* ==================== 阶段 2：创建 CUDA 设备与解码上下文 ==================== */
    int ret = av_hwdevice_ctx_create(&dec->hw_device_ctx,
                                     AV_HWDEVICE_TYPE_CUDA,
                                     NULL, NULL, 0);
    if (ret < 0) {
        print_av_error("av_hwdevice_ctx_create(CUDA)", ret);
        goto fail;
    }

    dec->codec_ctx = avcodec_alloc_context3(codec);
    if (!dec->codec_ctx)
        goto fail;

    dec->codec_ctx->opaque = dec;
    dec->codec_ctx->get_format = select_hw_format;
    dec->codec_ctx->hw_device_ctx = av_buffer_ref(dec->hw_device_ctx);
    dec->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dec->codec_ctx->thread_count = 1;
    if (!dec->codec_ctx->hw_device_ctx)
        goto fail;

    ret = avcodec_open2(dec->codec_ctx, codec, NULL);
    if (ret < 0) {
        print_av_error("avcodec_open2", ret);
        goto fail;
    }

    dec->rgb = malloc((size_t)SHOW_W * (size_t)SHOW_H * 3);
    if (!dec->rgb)
        goto fail;
    return dec;

fail:
    nvdec_dec_destroy(dec);
    return NULL;
}

void nvdec_dec_destroy(NvDecCtx *dec)
{
    if (!dec)
        return;
    sws_freeContext(dec->sws);
    free(dec->rgb);
    avcodec_free_context(&dec->codec_ctx);
    av_buffer_unref(&dec->hw_device_ctx);
    free(dec);
}

const uint8_t *nvdec_dec_rgb(const NvDecCtx *dec, int *show_w, int *show_h)
{
    if (!dec || !dec->rgb_ready)
        return NULL;
    if (show_w)
        *show_w = SHOW_W;
    if (show_h)
        *show_h = SHOW_H;
    return dec->rgb;
}

void nvdec_dec_last_timing(const NvDecCtx *dec, int64_t *hw_us, int64_t *show_us)
{
    if (hw_us)
        *hw_us = dec ? dec->last_hw_us : 0;
    if (show_us)
        *show_us = dec ? dec->last_show_us : 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int nvdec_dec_decode(NvDecCtx *dec, const uint8_t *data, size_t len,
                     int *width, int *height)
{
    if (!dec || !data || len == 0 || len > INT_MAX)
        return -1;

    dec->last_hw_us = 0;
    dec->last_show_us = 0;

    /* ==================== 阶段 3：提交一个完整 Annex-B AU ==================== */
    AVPacket *packet = av_packet_alloc();
    AVFrame *hw_frame = av_frame_alloc();
    AVFrame *sw_frame = av_frame_alloc();
    if (!packet || !hw_frame || !sw_frame)
        goto fail;

    int ret = av_new_packet(packet, (int)len);
    if (ret < 0) {
        print_av_error("av_new_packet", ret);
        goto fail;
    }
    memcpy(packet->data, data, len);

    uint64_t t0 = now_ns();
    ret = avcodec_send_packet(dec->codec_ctx, packet);
    if (ret < 0) {
        print_av_error("avcodec_send_packet", ret);
        goto fail;
    }

    /* ==================== 阶段 4：取得驻留在 GPU 上的解码帧 ==================== */
    ret = avcodec_receive_frame(dec->codec_ctx, hw_frame);
    uint64_t t1 = now_ns();
    dec->last_hw_us = (int64_t)(t1 - t0) / 1000;

    if (ret == AVERROR(EAGAIN)) {
        av_frame_free(&sw_frame);
        av_frame_free(&hw_frame);
        av_packet_free(&packet);
        return 0; /* AU 已被硬解码器接受，输出帧可能仍在重排队列中。 */
    }
    if (ret < 0) {
        print_av_error("avcodec_receive_frame", ret);
        goto fail;
    }
    if (hw_frame->format != dec->hw_pix_fmt) {
        fprintf(stderr, "decoder unexpectedly returned a software frame\n");
        goto fail;
    }

    if (width)
        *width = hw_frame->width;
    if (height)
        *height = hw_frame->height;

    /* ==================== 阶段 5：下载到 CPU 并缩放到 RGB24 预览 ==================== */
    uint64_t t2 = now_ns();
    ret = av_hwframe_transfer_data(sw_frame, hw_frame, 0);
    if (ret < 0) {
        print_av_error("av_hwframe_transfer_data", ret);
        goto fail;
    }

    if (!dec->sws ||
        dec->sws_src_fmt != sw_frame->format ||
        dec->sws_src_w != sw_frame->width ||
        dec->sws_src_h != sw_frame->height) {
        sws_freeContext(dec->sws);
        dec->sws = sws_getContext(sw_frame->width, sw_frame->height,
                                  sw_frame->format,
                                  SHOW_W, SHOW_H, AV_PIX_FMT_RGB24,
                                  SWS_BILINEAR, NULL, NULL, NULL);
        if (!dec->sws) {
            fprintf(stderr, "sws_getContext failed\n");
            goto fail;
        }
        dec->sws_src_fmt = sw_frame->format;
        dec->sws_src_w = sw_frame->width;
        dec->sws_src_h = sw_frame->height;
    }

    uint8_t *dst[4] = {dec->rgb, NULL, NULL, NULL};
    int dst_linesize[4] = {SHOW_W * 3, 0, 0, 0};
    sws_scale(dec->sws,
              (const uint8_t *const *)sw_frame->data, sw_frame->linesize,
              0, sw_frame->height, dst, dst_linesize);
    dec->rgb_ready = 1;
    uint64_t t3 = now_ns();
    dec->last_show_us = (int64_t)(t3 - t2) / 1000;

    av_frame_free(&sw_frame);
    av_frame_free(&hw_frame);
    av_packet_free(&packet);
    return 0;

fail:
    av_frame_free(&sw_frame);
    av_frame_free(&hw_frame);
    av_packet_free(&packet);
    return -1;
}
