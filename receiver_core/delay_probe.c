#include "delay_probe.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../common/delay_probe_protocol.h"

struct DelaySample {
    int64_t rtt_ns;
    int64_t offset_ns;
};

static int64_t clock_ns(clockid_t clock_id)
{
    struct timespec now;
    clock_gettime(clock_id, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static int compare_rtt(const void *left, const void *right)
{
    const struct DelaySample *a = left, *b = right;
    return (a->rtt_ns > b->rtt_ns) - (a->rtt_ns < b->rtt_ns);
}

static int compare_offset(const void *left, const void *right)
{
    const struct DelaySample *a = left, *b = right;
    return (a->offset_ns > b->offset_ns) - (a->offset_ns < b->offset_ns);
}

static int connect_udp(const char *host, int port)
{
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, port_text, &hints, &addresses) != 0)
        return -1;
    int fd = -1;
    for (struct addrinfo *it = addresses; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd >= 0 && connect(fd, it->ai_addr, it->ai_addrlen) == 0)
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

int delay_probe_check(const char *host, int port, int requested,
                      int interval_ms, int64_t threshold_us)
{
    if (!host || port <= 0 || requested < 5 || interval_ms < 0 ||
        threshold_us <= 0)
        return -1;
    int fd = connect_udp(host, port);
    if (fd < 0) {
        fprintf(stderr, "启动检查失败：无法连接远端延迟探测服务 %s:%d\n",
                host, port);
        return -1;
    }

    struct DelaySample *samples = calloc((size_t)requested, sizeof(*samples));
    if (!samples) {
        close(fd);
        return -1;
    }
    int valid = 0;
    uint64_t session = (uint64_t)clock_ns(CLOCK_MONOTONIC) ^
                       ((uint64_t)getpid() << 32);
    for (int sequence = 0; sequence < requested; sequence++) {
        DelayProbePacket request = {
            .type = UDP_RTSTREAM_DELAY_REQUEST,
            .session = session,
            .sequence = (uint32_t)sequence,
            .t1_ns = clock_ns(CLOCK_REALTIME),
        };
        uint8_t bytes[UDP_RTSTREAM_DELAY_PACKET_SIZE];
        delay_probe_encode(bytes, &request);
        if (send(fd, bytes, sizeof(bytes), 0) == (ssize_t)sizeof(bytes)) {
            struct pollfd event = {.fd = fd, .events = POLLIN};
            if (poll(&event, 1, 200) > 0) {
                ssize_t size = recv(fd, bytes, sizeof(bytes), 0);
                int64_t t4_ns = clock_ns(CLOCK_REALTIME);
                DelayProbePacket response;
                if (size > 0 &&
                    delay_probe_decode(bytes, (size_t)size, &response) == 0 &&
                    response.type == UDP_RTSTREAM_DELAY_RESPONSE &&
                    response.session == session &&
                    response.sequence == (uint32_t)sequence) {
                    int64_t rtt_ns = (t4_ns - response.t1_ns) -
                                     (response.t3_ns - response.t2_ns);
                    int64_t offset_ns = ((response.t2_ns - response.t1_ns) +
                                         (response.t3_ns - t4_ns)) / 2;
                    if (rtt_ns >= 0)
                        samples[valid++] = (struct DelaySample){rtt_ns, offset_ns};
                }
            }
        }
        struct timespec pause = {
            .tv_sec = interval_ms / 1000,
            .tv_nsec = (interval_ms % 1000) * 1000000L,
        };
        nanosleep(&pause, NULL);
    }
    close(fd);

    if (valid < 5) {
        fprintf(stderr, "启动检查失败：远端延迟探测仅收到 %d/%d 个有效样本\n",
                valid, requested);
        free(samples);
        return -1;
    }
    qsort(samples, (size_t)valid, sizeof(*samples), compare_rtt);
    int selected = valid < 10 ? valid : 10;
    int64_t minimum_rtt_ns = samples[0].rtt_ns;
    qsort(samples, (size_t)selected, sizeof(*samples), compare_offset);
    int64_t offset_ns = samples[selected / 2].offset_ns;
    int64_t conservative_us = (llabs(offset_ns) + minimum_rtt_ns / 2) / 1000;
    printf("启动延迟检查：有效样本 %d/%d，最小 RTT %.3f ms，"
           "时钟偏差 %.3f ms，保守上界 %.3f ms\n",
           valid, requested, minimum_rtt_ns / 1000000.0,
           offset_ns / 1000000.0, conservative_us / 1000.0);
    fflush(stdout);
    free(samples);
    if (conservative_us >= threshold_us) {
        fprintf(stderr, "启动检查失败：同步与链路延迟保守上界 %lld us，"
                "要求小于 %lld us；请检查两端 PTP 和网络链路\n",
                (long long)conservative_us, (long long)threshold_us);
        return -1;
    }
    return 0;
}
