#ifndef UDP_RTSTREAM_CAM_V4L2_H
#define UDP_RTSTREAM_CAM_V4L2_H

#include <stdint.h>
#include "mpp_buffer.h"

typedef struct CamV4l2 CamV4l2;

CamV4l2 *cam_v4l2_open(const char *device, int width, int height, int fps,
                       int bufcnt);
void cam_v4l2_close(CamV4l2 *cam);

/* Returns buffer index >= 0, or -1 on error. */
int cam_v4l2_get_frame(CamV4l2 *cam, uint64_t *pts_ns, uint32_t *sequence);
int cam_v4l2_put_frame(CamV4l2 *cam, int index);
MppBuffer cam_v4l2_buf(CamV4l2 *cam, int index);

#endif
