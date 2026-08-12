#include "mpp_enc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rockchip/rk_mpi.h"
#include "rockchip/mpp_packet.h"
#include "rockchip/mpp_frame.h"
#include "rockchip/rk_venc_cfg.h"

struct MppEncCtx {
    MppCtx ctx;
    MppApi *mpi;
    MppEncCfg cfg;
    MppBufferGroup buf_grp;
    MppBuffer pkt_buf;
    uint8_t *hdr;
    size_t hdr_len;
    uint8_t *out;
    size_t out_cap;
    int width;
    int height;
    int hor_stride;
    int ver_stride;
    size_t pkt_size;
};

MppEncCtx *mpp_enc_create(int width, int height, int fps, int bps)
{
    MppEncCtx *enc = calloc(1, sizeof(*enc));
    if (!enc)
        return NULL;

    enc->width = width;
    enc->height = height;
    enc->hor_stride = (width + 15) & ~15;
    enc->ver_stride = (height + 15) & ~15;
    enc->pkt_size = enc->hor_stride * enc->ver_stride;

    MPP_RET ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret) {
        fprintf(stderr, "mpp_create failed %d\n", ret);
        goto fail;
    }

    ret = mpp_init(enc->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret) {
        fprintf(stderr, "mpp_init failed %d\n", ret);
        goto fail;
    }

    ret = mpp_enc_cfg_init(&enc->cfg);
    if (ret)
        goto fail;

    ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_CFG, enc->cfg);
    if (ret)
        goto fail;

    mpp_enc_cfg_set_s32(enc->cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:hor_stride", enc->hor_stride);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:ver_stride", enc->ver_stride);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:format", MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(enc->cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_max", bps * 17 / 16);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_min", bps * 15 / 16);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:gop", 1);
    mpp_enc_cfg_set_u32(enc->cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);

    mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_init", -1);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_max", 51);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_min", 10);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_max_i", 51);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_min_i", 10);

    mpp_enc_cfg_set_s32(enc->cfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(enc->cfg, "h264:level", 51);
    mpp_enc_cfg_set_s32(enc->cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(enc->cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(enc->cfg, "h264:trans8x8", 1);
    mpp_enc_cfg_set_s32(enc->cfg, "codec:type", MPP_VIDEO_CodingAVC);

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, enc->cfg);
    if (ret) {
        fprintf(stderr, "MPP_ENC_SET_CFG failed %d\n", ret);
        goto fail;
    }

    ret = mpp_buffer_group_get_internal(&enc->buf_grp, MPP_BUFFER_TYPE_DRM);
    if (ret)
        ret = mpp_buffer_group_get_internal(&enc->buf_grp, MPP_BUFFER_TYPE_ION);
    if (ret) {
        fprintf(stderr, "mpp_buffer_group_get failed %d\n", ret);
        goto fail;
    }

    ret = mpp_buffer_get(enc->buf_grp, &enc->pkt_buf, enc->pkt_size);
    if (ret) {
        fprintf(stderr, "mpp_buffer_get pkt failed %d\n", ret);
        goto fail;
    }

    {
        MppPacket packet = NULL;
        mpp_packet_init_with_buffer(&packet, enc->pkt_buf);
        mpp_packet_set_length(packet, 0);
        ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_HDR_SYNC, packet);
        if (ret) {
            fprintf(stderr, "MPP_ENC_GET_HDR_SYNC failed %d\n", ret);
            mpp_packet_deinit(&packet);
            goto fail;
        }
        void *ptr = mpp_packet_get_pos(packet);
        size_t len = mpp_packet_get_length(packet);
        if (!ptr || len == 0) {
            fprintf(stderr, "empty encoder header\n");
            mpp_packet_deinit(&packet);
            goto fail;
        }
        enc->hdr = malloc(len);
        if (!enc->hdr) {
            mpp_packet_deinit(&packet);
            goto fail;
        }
        memcpy(enc->hdr, ptr, len);
        enc->hdr_len = len;
        fprintf(stderr, "cached SPS/PPS %zu bytes\n", enc->hdr_len);
        mpp_packet_deinit(&packet);
    }

    enc->out_cap = enc->pkt_size + enc->hdr_len + 4096;
    enc->out = malloc(enc->out_cap);
    if (!enc->out)
        goto fail;

    return enc;

fail:
    mpp_enc_destroy(enc);
    return NULL;
}

void mpp_enc_destroy(MppEncCtx *enc)
{
    if (!enc)
        return;
    free(enc->hdr);
    free(enc->out);
    if (enc->pkt_buf)
        mpp_buffer_put(enc->pkt_buf);
    if (enc->buf_grp)
        mpp_buffer_group_put(enc->buf_grp);
    if (enc->cfg)
        mpp_enc_cfg_deinit(enc->cfg);
    if (enc->ctx) {
        enc->mpi->reset(enc->ctx);
        mpp_destroy(enc->ctx);
    }
    free(enc);
}

static int annexb_nal_type(const uint8_t *data, size_t len, int type)
{
    for (size_t i = 0; i + 4 < len; i++) {
        int sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            sc = 3;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            sc = 4;
        if (!sc)
            continue;
        if ((data[i + sc] & 0x1f) == type)
            return 1;
    }
    return 0;
}

static int annexb_has_sps(const uint8_t *data, size_t len)
{
    return annexb_nal_type(data, len, 7);
}

static int annexb_is_key(const uint8_t *data, size_t len)
{
    /* IDR or SPS indicates random-access unit */
    return annexb_nal_type(data, len, 5) || annexb_nal_type(data, len, 7);
}

int mpp_enc_encode(MppEncCtx *enc, MppBuffer frame_buf,
                   const uint8_t **out_data, size_t *out_len, int *keyframe)
{
    MppFrame frame = NULL;
    MppPacket packet = NULL;
    MPP_RET ret;

    ret = mpp_frame_init(&frame);
    if (ret)
        return -1;

    mpp_frame_set_width(frame, enc->width);
    mpp_frame_set_height(frame, enc->height);
    mpp_frame_set_hor_stride(frame, enc->hor_stride);
    mpp_frame_set_ver_stride(frame, enc->ver_stride);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, frame_buf);

    MppMeta meta = mpp_frame_get_meta(frame);
    mpp_packet_init_with_buffer(&packet, enc->pkt_buf);
    mpp_packet_set_length(packet, 0);
    mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);

    ret = enc->mpi->encode_put_frame(enc->ctx, frame);
    mpp_frame_deinit(&frame);
    if (ret) {
        fprintf(stderr, "encode_put_frame %d\n", ret);
        mpp_packet_deinit(&packet);
        return -1;
    }

    ret = enc->mpi->encode_get_packet(enc->ctx, &packet);
    if (ret || !packet) {
        fprintf(stderr, "encode_get_packet %d\n", ret);
        return -1;
    }

    const uint8_t *src = mpp_packet_get_pos(packet);
    size_t src_len = mpp_packet_get_length(packet);
    int is_key = annexb_is_key(src, src_len);
    size_t need = src_len;
    /* Only attach SPS/PPS on key frames when missing */
    int prepend = is_key && enc->hdr_len > 0 && !annexb_has_sps(src, src_len);
    if (prepend)
        need += enc->hdr_len;

    if (need > enc->out_cap) {
        uint8_t *nbuf = realloc(enc->out, need);
        if (!nbuf) {
            mpp_packet_deinit(&packet);
            return -1;
        }
        enc->out = nbuf;
        enc->out_cap = need;
    }

    size_t off = 0;
    if (prepend) {
        memcpy(enc->out, enc->hdr, enc->hdr_len);
        off = enc->hdr_len;
        is_key = 1;
    }
    memcpy(enc->out + off, src, src_len);
    off += src_len;

    *out_data = enc->out;
    *out_len = off;
    if (keyframe)
        *keyframe = is_key;

    mpp_packet_deinit(&packet);
    return 0;
}
