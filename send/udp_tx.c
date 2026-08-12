#include "udp_tx.h"
#include "proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct UdpTx {
    int fd;
    struct sockaddr_in addr;
    uint32_t seq;
};

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
}

UdpTx *udp_tx_open(const char *ip, int port)
{
    UdpTx *tx = calloc(1, sizeof(*tx));
    if (!tx)
        return NULL;

    tx->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx->fd < 0) {
        perror("socket");
        free(tx);
        return NULL;
    }

    int buf = 8 * 1024 * 1024;
    setsockopt(tx->fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));

    memset(&tx->addr, 0, sizeof(tx->addr));
    tx->addr.sin_family = AF_INET;
    tx->addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &tx->addr.sin_addr) != 1) {
        fprintf(stderr, "bad ip %s\n", ip);
        close(tx->fd);
        free(tx);
        return NULL;
    }

    return tx;
}

void udp_tx_close(UdpTx *tx)
{
    if (!tx)
        return;
    if (tx->fd >= 0)
        close(tx->fd);
    free(tx);
}

int udp_tx_send_au(UdpTx *tx, const uint8_t *data, size_t len,
                   uint32_t frame_id, uint64_t pts_ns, int keyframe, int cam_id)
{
    if (!tx || !data || len == 0)
        return -1;
    if (cam_id < 0 || cam_id > 3)
        cam_id = 0;

    uint16_t frag_cnt = (uint16_t)((len + URTS_PAYLOAD_MAX - 1) / URTS_PAYLOAD_MAX);
    if (frag_cnt == 0)
        frag_cnt = 1;

    uint8_t pkt[URTS_PKT_MAX];
    size_t offset = 0;

    for (uint16_t i = 0; i < frag_cnt; i++) {
        size_t remain = len - offset;
        uint16_t plen = remain > URTS_PAYLOAD_MAX ? URTS_PAYLOAD_MAX : (uint16_t)remain;
        uint8_t flags = 0;
        if (keyframe)
            flags |= URTS_FLAG_KEY;
        if (i + 1 == frag_cnt)
            flags |= URTS_FLAG_LAST;
        flags = (uint8_t)URTS_FLAG_SET_CAM(flags, cam_id);

        pkt[0] = URTS_MAGIC0;
        pkt[1] = URTS_MAGIC1;
        pkt[2] = URTS_MAGIC2;
        pkt[3] = URTS_MAGIC3;
        pkt[4] = URTS_VERSION;
        pkt[5] = flags;
        put_be32(pkt + 6, tx->seq++);
        put_be32(pkt + 10, frame_id);
        put_be64(pkt + 14, pts_ns);
        put_be16(pkt + 22, i);
        put_be16(pkt + 24, frag_cnt);
        put_be16(pkt + 26, plen);
        memcpy(pkt + URTS_HEADER_SIZE, data + offset, plen);

        ssize_t n = sendto(tx->fd, pkt, URTS_HEADER_SIZE + plen, 0,
                           (struct sockaddr *)&tx->addr, sizeof(tx->addr));
        if (n < 0) {
            perror("sendto");
            return -1;
        }
        offset += plen;
    }
    return 0;
}
