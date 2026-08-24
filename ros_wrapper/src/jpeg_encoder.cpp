#include "jpeg_encoder.h"

#include <cstdio>
#include <cstdlib>
#include <jpeglib.h>

bool encode_rgb_to_jpeg(const uint8_t *rgb, int width, int height,
                        std::vector<uint8_t> &jpeg)
{
    /* ==================== 阶段 1：校验 RGB 输入 ==================== */
    if (!rgb || width <= 0 || height <= 0)
        return false;

    /* ==================== 阶段 2：配置内存 JPEG 编码器 ==================== */
    jpeg_compress_struct compressor{};
    jpeg_error_mgr error_manager{};
    compressor.err = jpeg_std_error(&error_manager);
    jpeg_create_compress(&compressor);

    unsigned char *encoded_data = nullptr;
    unsigned long encoded_size = 0;
    jpeg_mem_dest(&compressor, &encoded_data, &encoded_size);
    compressor.image_width = static_cast<JDIMENSION>(width);
    compressor.image_height = static_cast<JDIMENSION>(height);
    compressor.input_components = 3;
    compressor.in_color_space = JCS_RGB;
    jpeg_set_defaults(&compressor);
    jpeg_set_quality(&compressor, 85, TRUE);

    /* ==================== 阶段 3：逐行压缩并复制结果 ==================== */
    jpeg_start_compress(&compressor, TRUE);
    const int row_stride = width * 3;
    while (compressor.next_scanline < compressor.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(
            rgb + static_cast<size_t>(compressor.next_scanline) * row_stride);
        jpeg_write_scanlines(&compressor, &row, 1);
    }
    jpeg_finish_compress(&compressor);
    jpeg.assign(encoded_data, encoded_data + encoded_size);

    std::free(encoded_data);
    jpeg_destroy_compress(&compressor);
    return !jpeg.empty();
}
