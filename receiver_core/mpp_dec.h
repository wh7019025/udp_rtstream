#ifndef UDP_RTSTREAM_MPP_DEC_H
#define UDP_RTSTREAM_MPP_DEC_H

#include <stddef.h>
#include <stdint.h>

typedef struct MppDecCtx MppDecCtx;

MppDecCtx *mpp_dec_create(void);
void mpp_dec_destroy(MppDecCtx *dec);

/* Decode one Annex-B AU. Returns 0 on got frame, -1 on error.
 * Sets width/height when a decoded frame is produced. */
int mpp_dec_decode(MppDecCtx *dec, const uint8_t *data, size_t len,
                   int *width, int *height);

#endif
