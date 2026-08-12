#ifndef UDP_RTSTREAM_MPP_ENC_H
#define UDP_RTSTREAM_MPP_ENC_H

#include <stddef.h>
#include <stdint.h>
#include "mpp_buffer.h"

typedef struct MppEncCtx MppEncCtx;

MppEncCtx *mpp_enc_create(int width, int height, int fps, int bps);
void mpp_enc_destroy(MppEncCtx *enc);

/* Encode one NV12 dma frame. On success returns packet bytes in *out_len.
 * *out_data is valid until next encode call. keyframe set if IDR/I.
 * SPS/PPS are prepended to each AU. */
int mpp_enc_encode(MppEncCtx *enc, MppBuffer frame_buf,
                   const uint8_t **out_data, size_t *out_len, int *keyframe);

#endif
