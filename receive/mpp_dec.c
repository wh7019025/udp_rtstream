#include "mpp_dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rk_mpi.h"
#include "mpp_packet.h"
#include "mpp_frame.h"
#include "rk_mpi_cmd.h"

struct MppDecCtx {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup frm_grp;
    int info_ready;
};

MppDecCtx *mpp_dec_create(void)
{
    MppDecCtx *dec = calloc(1, sizeof(*dec));
    if (!dec)
        return NULL;

    MPP_RET ret = mpp_create(&dec->ctx, &dec->mpi);
    if (ret) {
        fprintf(stderr, "mpp_create failed %d\n", ret);
        goto fail;
    }

    ret = mpp_init(dec->ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    if (ret) {
        fprintf(stderr, "mpp_init dec failed %d\n", ret);
        goto fail;
    }

    MppDecCfg cfg = NULL;
    ret = mpp_dec_cfg_init(&cfg);
    if (ret)
        goto fail;
    ret = dec->mpi->control(dec->ctx, MPP_DEC_GET_CFG, cfg);
    if (ret == MPP_OK) {
        mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
        ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_CFG, cfg);
    }
    mpp_dec_cfg_deinit(cfg);
    if (ret) {
        fprintf(stderr, "MPP_DEC_SET_CFG failed %d\n", ret);
        goto fail;
    }

    RK_S32 timeout = 100;
    ret = dec->mpi->control(dec->ctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout);
    if (ret)
        fprintf(stderr, "SET_OUTPUT_TIMEOUT warn %d\n", ret);

    return dec;

fail:
    mpp_dec_destroy(dec);
    return NULL;
}

void mpp_dec_destroy(MppDecCtx *dec)
{
    if (!dec)
        return;
    if (dec->frm_grp)
        mpp_buffer_group_put(dec->frm_grp);
    if (dec->ctx) {
        dec->mpi->reset(dec->ctx);
        mpp_destroy(dec->ctx);
    }
    free(dec);
}

static int handle_info_change(MppDecCtx *dec, MppFrame frame)
{
    size_t buf_size = mpp_frame_get_buf_size(frame);
    RK_U32 w = mpp_frame_get_width(frame);
    RK_U32 h = mpp_frame_get_height(frame);

    fprintf(stderr, "dec info change %ux%u buf_size=%zu\n", w, h, buf_size);
    fflush(stderr);

    if (dec->frm_grp) {
        mpp_buffer_group_put(dec->frm_grp);
        dec->frm_grp = NULL;
    }

    MPP_RET ret = mpp_buffer_group_get_internal(&dec->frm_grp, MPP_BUFFER_TYPE_ION);
    if (ret)
        ret = mpp_buffer_group_get_internal(&dec->frm_grp, MPP_BUFFER_TYPE_DRM);
    if (ret) {
        fprintf(stderr, "frm_grp get failed %d\n", ret);
        return -1;
    }

    mpp_buffer_group_limit_config(dec->frm_grp, buf_size, 24);

    ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_EXT_BUF_GROUP, dec->frm_grp);
    if (ret) {
        fprintf(stderr, "SET_EXT_BUF_GROUP failed %d\n", ret);
        return -1;
    }

    ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
    if (ret) {
        fprintf(stderr, "INFO_CHANGE_READY failed %d\n", ret);
        return -1;
    }
    dec->info_ready = 1;
    return 0;
}

int mpp_dec_decode(MppDecCtx *dec, const uint8_t *data, size_t len,
                   int *width, int *height)
{
    if (!dec || !data || len == 0)
        return -1;

    /* Own a copy: MPP may still reference packet memory after put returns. */
    uint8_t *copy = malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, data, len);

    MppPacket packet = NULL;
    MPP_RET ret = mpp_packet_init(&packet, copy, len);
    if (ret) {
        free(copy);
        return -1;
    }
    mpp_packet_set_size(packet, len);
    mpp_packet_set_pos(packet, copy);
    mpp_packet_set_length(packet, len);

    int pkt_done = 0;
    int got_frame = 0;
    int tries = 0;
    const int max_tries = 64;

    while (!got_frame && tries++ < max_tries) {
        if (!pkt_done) {
            ret = dec->mpi->decode_put_packet(dec->ctx, packet);
            if (ret == MPP_OK)
                pkt_done = 1;
            else {
                usleep(1000);
                continue;
            }
        }

        MppFrame frame = NULL;
        ret = dec->mpi->decode_get_frame(dec->ctx, &frame);
        if (ret || !frame) {
            /* With BLOCK timeout a NULL frame is unexpected; avoid spinning forever. */
            usleep(1000);
            if (tries > 8 && !dec->info_ready)
                break;
            continue;
        }

        if (mpp_frame_get_info_change(frame)) {
            int hc = handle_info_change(dec, frame);
            mpp_frame_deinit(&frame);
            if (hc < 0)
                break;

            /* Resubmit AU after buffers are ready. */
            mpp_packet_set_pos(packet, copy);
            mpp_packet_set_length(packet, len);
            pkt_done = 0;
            tries = 0;
            continue;
        }

        if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame)) {
            mpp_frame_deinit(&frame);
            continue;
        }

        if (width)
            *width = (int)mpp_frame_get_width(frame);
        if (height)
            *height = (int)mpp_frame_get_height(frame);
        mpp_frame_deinit(&frame);
        got_frame = 1;
    }

    mpp_packet_deinit(&packet);
    free(copy);
    return got_frame ? 0 : -1;
}
