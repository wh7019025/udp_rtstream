#include "delay_probe_responder.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../common/delay_probe_protocol.h"

struct DelayProbeResponder {
    int socket_fd;
    int port;
    volatile int running;
};

static int64_t realtime_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

DelayProbeResponder *delay_probe_responder_create(int port)
{
    if (port < 1024 || port > 65535)
        return NULL;
    DelayProbeResponder *responder = calloc(1, sizeof(*responder));
    if (!responder)
        return NULL;
    responder->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    responder->port = port;
    responder->running = 1;
    if (responder->socket_fd < 0)
        goto fail;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (bind(responder->socket_fd, (struct sockaddr *)&address,
             sizeof(address)) != 0) {
        perror("delay probe bind");
        goto fail;
    }
    return responder;

fail:
    delay_probe_responder_destroy(responder);
    return NULL;
}

void delay_probe_responder_run(DelayProbeResponder *responder)
{
    printf("delay probe responder listening UDP %d\n", responder->port);
    fflush(stdout);
    while (responder->running) {
        struct pollfd event = {
            .fd = responder->socket_fd,
            .events = POLLIN,
        };
        int ready = poll(&event, 1, 200);
        if (ready == 0)
            continue;
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        uint8_t bytes[UDP_RTSTREAM_DELAY_PACKET_SIZE];
        struct sockaddr_storage peer;
        socklen_t peer_size = sizeof(peer);
        ssize_t size = recvfrom(responder->socket_fd, bytes, sizeof(bytes), 0,
                                (struct sockaddr *)&peer, &peer_size);
        int64_t t2_ns = realtime_ns();
        if (size < 0)
            continue;
        DelayProbePacket packet;
        if (delay_probe_decode(bytes, (size_t)size, &packet) != 0 ||
            packet.type != UDP_RTSTREAM_DELAY_REQUEST)
            continue;
        packet.type = UDP_RTSTREAM_DELAY_RESPONSE;
        packet.t2_ns = t2_ns;
        packet.t3_ns = realtime_ns();
        delay_probe_encode(bytes, &packet);
        sendto(responder->socket_fd, bytes, sizeof(bytes), 0,
               (struct sockaddr *)&peer, peer_size);
    }
}

void delay_probe_responder_stop(DelayProbeResponder *responder)
{
    if (responder)
        responder->running = 0;
}

void delay_probe_responder_destroy(DelayProbeResponder *responder)
{
    if (!responder)
        return;
    if (responder->socket_fd >= 0)
        close(responder->socket_fd);
    free(responder);
}
