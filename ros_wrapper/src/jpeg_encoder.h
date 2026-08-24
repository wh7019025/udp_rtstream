#ifndef UDP_RTSTREAM_JPEG_ENCODER_H
#define UDP_RTSTREAM_JPEG_ENCODER_H

#include <cstdint>
#include <vector>

bool encode_rgb_to_jpeg(const uint8_t *rgb, int width, int height,
                        std::vector<uint8_t> &jpeg);

#endif
