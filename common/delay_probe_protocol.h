#ifndef UDP_RTSTREAM_DELAY_PROBE_PROTOCOL_H
#define UDP_RTSTREAM_DELAY_PROBE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define UDP_RTSTREAM_DELAY_MAGIC 0x55524450U /* URDP */
#define UDP_RTSTREAM_DELAY_VERSION 1
#define UDP_RTSTREAM_DELAY_REQUEST 1
#define UDP_RTSTREAM_DELAY_RESPONSE 2
#define UDP_RTSTREAM_DELAY_PACKET_SIZE 48

typedef struct {
    uint8_t type;
    uint64_t session;
    uint32_t sequence;
    int64_t t1_ns;
    int64_t t2_ns;
    int64_t t3_ns;
} DelayProbePacket;

static inline void delay_put_u32(uint8_t *out, uint32_t value)
{
    for (int i = 3; i >= 0; i--)
        *out++ = (uint8_t)(value >> (i * 8));
}

static inline void delay_put_u64(uint8_t *out, uint64_t value)
{
    for (int i = 7; i >= 0; i--)
        *out++ = (uint8_t)(value >> (i * 8));
}

static inline uint32_t delay_get_u32(const uint8_t *in)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value = (value << 8) | in[i];
    return value;
}

static inline uint64_t delay_get_u64(const uint8_t *in)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value = (value << 8) | in[i];
    return value;
}

static inline void delay_probe_encode(uint8_t out[UDP_RTSTREAM_DELAY_PACKET_SIZE],
                                      const DelayProbePacket *packet)
{
    for (size_t i = 0; i < UDP_RTSTREAM_DELAY_PACKET_SIZE; i++)
        out[i] = 0;
    delay_put_u32(out, UDP_RTSTREAM_DELAY_MAGIC);
    out[4] = UDP_RTSTREAM_DELAY_VERSION;
    out[5] = packet->type;
    delay_put_u64(out + 8, packet->session);
    delay_put_u32(out + 16, packet->sequence);
    delay_put_u64(out + 24, (uint64_t)packet->t1_ns);
    delay_put_u64(out + 32, (uint64_t)packet->t2_ns);
    delay_put_u64(out + 40, (uint64_t)packet->t3_ns);
}

static inline int delay_probe_decode(const uint8_t *data, size_t size,
                                     DelayProbePacket *packet)
{
    if (size != UDP_RTSTREAM_DELAY_PACKET_SIZE ||
        delay_get_u32(data) != UDP_RTSTREAM_DELAY_MAGIC ||
        data[4] != UDP_RTSTREAM_DELAY_VERSION ||
        (data[5] != UDP_RTSTREAM_DELAY_REQUEST &&
         data[5] != UDP_RTSTREAM_DELAY_RESPONSE))
        return -1;
    packet->type = data[5];
    packet->session = delay_get_u64(data + 8);
    packet->sequence = delay_get_u32(data + 16);
    packet->t1_ns = (int64_t)delay_get_u64(data + 24);
    packet->t2_ns = (int64_t)delay_get_u64(data + 32);
    packet->t3_ns = (int64_t)delay_get_u64(data + 40);
    return 0;
}

#endif
