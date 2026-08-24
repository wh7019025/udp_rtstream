#ifndef UDP_RTSTREAM_DELAY_PROBE_RESPONDER_H
#define UDP_RTSTREAM_DELAY_PROBE_RESPONDER_H

typedef struct DelayProbeResponder DelayProbeResponder;

DelayProbeResponder *delay_probe_responder_create(int port);
void delay_probe_responder_run(DelayProbeResponder *responder);
void delay_probe_responder_stop(DelayProbeResponder *responder);
void delay_probe_responder_destroy(DelayProbeResponder *responder);

#endif
