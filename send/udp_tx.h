#ifndef UDP_RTSTREAM_UDP_TX_H
#define UDP_RTSTREAM_UDP_TX_H

#include <stddef.h>
#include <stdint.h>

typedef struct UdpTx UdpTx;

UdpTx *udp_tx_open(const char *ip, int port);
void udp_tx_close(UdpTx *tx);

/* Fragment and send one access unit. cam_id: 0 or 1.
 * enc_done_ns: CLOCK_REALTIME when encode finished. */
int udp_tx_send_au(UdpTx *tx, const uint8_t *data, size_t len,
                   uint32_t frame_id, uint64_t pts_ns, uint64_t enc_done_ns,
                   int keyframe, int cam_id);

#endif
