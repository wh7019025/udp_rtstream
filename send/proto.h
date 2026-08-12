#ifndef UDP_RTSTREAM_PROTO_H
#define UDP_RTSTREAM_PROTO_H

#include <stdint.h>

#define URTS_MAGIC0 'U'
#define URTS_MAGIC1 'R'
#define URTS_MAGIC2 'T'
#define URTS_MAGIC3 'S'
#define URTS_VERSION 2

#define URTS_FLAG_KEY   0x01
#define URTS_FLAG_LAST  0x02
#define URTS_FLAG_CAM_SHIFT 2
#define URTS_FLAG_CAM_MASK  0x0c

#define URTS_CAM_ID(flags) (((flags) & URTS_FLAG_CAM_MASK) >> URTS_FLAG_CAM_SHIFT)
#define URTS_FLAG_SET_CAM(flags, cam) \
    (((flags) & ~URTS_FLAG_CAM_MASK) | (((cam) << URTS_FLAG_CAM_SHIFT) & URTS_FLAG_CAM_MASK))

#define URTS_PAYLOAD_MAX 1400
#define URTS_HEADER_SIZE 44
#define URTS_PKT_MAX     (URTS_HEADER_SIZE + URTS_PAYLOAD_MAX)

#pragma pack(push, 1)
typedef struct {
    char     magic[4];
    uint8_t  version;
    uint8_t  flags;
    uint32_t seq;
    uint32_t frame_id;
    uint64_t pts_ns;
    uint16_t frag_idx;
    uint16_t frag_cnt;
    uint16_t payload_len;
    uint64_t enc_done_ns;
    uint64_t frag_tx_ns;
} UrtsHeader;
#pragma pack(pop)

_Static_assert(sizeof(UrtsHeader) == URTS_HEADER_SIZE, "UrtsHeader size");

#endif
