#include "receiver_core.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"

#define RECEIVER_MAX_AU (8 * 1024 * 1024)

struct CameraAssembly {
    uint8_t *au;
    uint32_t frame_id;
    uint16_t next_fragment;
    uint16_t fragment_count;
    size_t au_size;
    uint64_t pts_ns;
    uint64_t enc_done_ns;
    uint64_t first_rx_ns;
    int keyframe;
};

struct ReceiverCore {
    int socket_fd;
    int camera_count;
    struct CameraAssembly *cameras;
};

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t read_be64(const uint8_t *p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value = (value << 8) | p[i];
    return value;
}

static uint64_t realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void reset_assembly(struct CameraAssembly *camera, uint32_t frame_id,
                           uint16_t fragment_count, uint64_t pts_ns,
                           uint64_t enc_done_ns, uint64_t first_rx_ns,
                           int keyframe)
{
    camera->frame_id = frame_id;
    camera->next_fragment = 0;
    camera->fragment_count = fragment_count;
    camera->au_size = 0;
    camera->pts_ns = pts_ns;
    camera->enc_done_ns = enc_done_ns;
    camera->first_rx_ns = first_rx_ns;
    camera->keyframe = keyframe;
}

ReceiverCore *receiver_core_create(int udp_port, int camera_count)
{
    /* ==================== 阶段 1：校验配置并分配重组缓存 ==================== */
    if (udp_port <= 0 || udp_port > 65535 || camera_count <= 0 || camera_count > 16)
        return NULL;

    ReceiverCore *receiver = calloc(1, sizeof(*receiver));
    if (!receiver)
        return NULL;
    receiver->socket_fd = -1;
    receiver->camera_count = camera_count;
    receiver->cameras = calloc((size_t)camera_count, sizeof(*receiver->cameras));
    if (!receiver->cameras)
        goto fail;

    for (int i = 0; i < camera_count; i++) {
        receiver->cameras[i].frame_id = UINT32_MAX;
        receiver->cameras[i].au = malloc(RECEIVER_MAX_AU);
        if (!receiver->cameras[i].au)
            goto fail;
    }

    /* ==================== 阶段 2：创建并绑定 UDP 接收端口 ==================== */
    receiver->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver->socket_fd < 0)
        goto fail;

    int buffer_size = 32 * 1024 * 1024;
    int reuse_address = 1;
    setsockopt(receiver->socket_fd, SOL_SOCKET, SO_RCVBUF,
               &buffer_size, sizeof(buffer_size));
    setsockopt(receiver->socket_fd, SOL_SOCKET, SO_REUSEADDR,
               &reuse_address, sizeof(reuse_address));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)udp_port);
    if (bind(receiver->socket_fd, (struct sockaddr *)&address,
             sizeof(address)) < 0) {
        perror("receiver bind");
        goto fail;
    }
    return receiver;

fail:
    receiver_core_destroy(receiver);
    return NULL;
}

void receiver_core_destroy(ReceiverCore *receiver)
{
    if (!receiver)
        return;
    if (receiver->socket_fd >= 0)
        close(receiver->socket_fd);
    if (receiver->cameras) {
        for (int i = 0; i < receiver->camera_count; i++)
            free(receiver->cameras[i].au);
    }
    free(receiver->cameras);
    free(receiver);
}

int receiver_core_poll(ReceiverCore *receiver, int timeout_ms,
                       ReceiverFrameCallback callback, void *user)
{
    /* ==================== 阶段 3：等待并校验一个 UDP 分片 ==================== */
    if (!receiver || !callback || timeout_ms < 0)
        return -1;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(receiver->socket_fd, &read_fds);
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int ready = select(receiver->socket_fd + 1, &read_fds, NULL, NULL, &timeout);
    if (ready == 0)
        return 0;
    if (ready < 0)
        return errno == EINTR ? 0 : -1;

    uint8_t packet[URTS_PKT_MAX];
    ssize_t packet_size = recvfrom(receiver->socket_fd, packet,
                                   sizeof(packet), 0, NULL, NULL);
    if (packet_size < URTS_HEADER_SIZE)
        return packet_size < 0 && errno != EINTR ? -1 : 0;
    if (packet[0] != URTS_MAGIC0 || packet[1] != URTS_MAGIC1 ||
        packet[2] != URTS_MAGIC2 || packet[3] != URTS_MAGIC3 ||
        packet[4] != URTS_VERSION)
        return 0;

    uint8_t flags = packet[5];
    int cam_id = (int)URTS_CAM_ID(flags);
    uint32_t frame_id = read_be32(packet + 10);
    uint64_t pts_ns = read_be64(packet + 14);
    uint16_t fragment_index = read_be16(packet + 22);
    uint16_t fragment_count = read_be16(packet + 24);
    uint16_t payload_size = read_be16(packet + 26);
    uint64_t enc_done_ns = read_be64(packet + 28);
    uint64_t frag_tx_ns = read_be64(packet + 36);
    uint64_t packet_rx_ns = realtime_ns();

    if (cam_id < 0 || cam_id >= receiver->camera_count ||
        fragment_count == 0 || fragment_index >= fragment_count ||
        (size_t)packet_size < URTS_HEADER_SIZE + (size_t)payload_size)
        return 0;

    /* ==================== 阶段 4：按相机和帧号重组完整 AU ==================== */
    struct CameraAssembly *camera = &receiver->cameras[cam_id];
    if (frame_id != camera->frame_id) {
        reset_assembly(camera, frame_id, fragment_count, pts_ns,
                       enc_done_ns, packet_rx_ns,
                       (flags & URTS_FLAG_KEY) != 0);
    }

    if (fragment_index != camera->next_fragment ||
        fragment_count != camera->fragment_count) {
        if (fragment_index != 0) {
            camera->next_fragment = 0;
            camera->au_size = 0;
            return 0;
        }
        reset_assembly(camera, frame_id, fragment_count, pts_ns,
                       enc_done_ns, packet_rx_ns,
                       (flags & URTS_FLAG_KEY) != 0);
    }

    if (camera->au_size + payload_size > RECEIVER_MAX_AU) {
        camera->next_fragment = 0;
        camera->au_size = 0;
        return 0;
    }
    memcpy(camera->au + camera->au_size,
           packet + URTS_HEADER_SIZE, payload_size);
    camera->au_size += payload_size;
    camera->next_fragment++;

    /* ==================== 阶段 5：向上层交付完整编码帧 ==================== */
    if (!(flags & URTS_FLAG_LAST))
        return 0;
    if (camera->next_fragment != camera->fragment_count) {
        camera->next_fragment = 0;
        camera->au_size = 0;
        return 0;
    }

    ReceiverFrame frame = {
        .cam_id = cam_id,
        .frame_id = frame_id,
        .pts_ns = camera->pts_ns,
        .enc_done_ns = camera->enc_done_ns,
        .last_frag_tx_ns = frag_tx_ns,
        .first_rx_ns = camera->first_rx_ns,
        .recv_done_ns = packet_rx_ns,
        .keyframe = camera->keyframe,
        .data = camera->au,
        .size = camera->au_size,
    };
    callback(&frame, user);
    camera->next_fragment = 0;
    camera->au_size = 0;
    return 1;
}
