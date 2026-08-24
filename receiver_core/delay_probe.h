#ifndef UDP_RTSTREAM_DELAY_PROBE_H
#define UDP_RTSTREAM_DELAY_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int delay_probe_check(const char *host, int port, int samples,
                      int interval_ms, int64_t threshold_us);

#ifdef __cplusplus
}
#endif

#endif
