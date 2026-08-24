#ifndef UDP_RTSTREAM_NVDEC_DEC_H
#define UDP_RTSTREAM_NVDEC_DEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvDecCtx NvDecCtx;

NvDecCtx *nvdec_dec_create(void);
void nvdec_dec_destroy(NvDecCtx *dec);

/* Decode one H.264 Annex-B AU with NVIDIA NVDEC.
 * On success, also refreshes an RGB24 preview buffer (see nvdec_dec_rgb). */
int nvdec_dec_decode(NvDecCtx *dec, const uint8_t *data, size_t len,
                     int *width, int *height);

/* Last RGB24 preview (show_w * show_h * 3). Valid until next decode/destroy. */
const uint8_t *nvdec_dec_rgb(const NvDecCtx *dec, int *show_w, int *show_h);

/* Microseconds from the last decode call: NVDEC vs GPU->CPU+scale preview. */
void nvdec_dec_last_timing(const NvDecCtx *dec, int64_t *hw_us, int64_t *show_us);

#ifdef __cplusplus
}
#endif

#endif
