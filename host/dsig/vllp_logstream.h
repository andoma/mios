#pragma once

#include <stdint.h>
#include <stddef.h>

struct vllp;
typedef struct vllp_logstream vllp_logstream_t;

struct vllp_logstream *vllp_logstream_create(struct vllp *v, void *opaque,
                                             void (*cb)(void *opaque,
                                                        int level, uint32_t sequence,
                                                        int64_t ms_ago, const char *msg));

void vllp_logstream_destroy(struct vllp_logstream *vl);

// Decode one frame received on the "log" channel. *seq carries the
// sequence number from one frame to the next. On success msg points
// into data (not NUL terminated). Returns -1 if the frame is truncated
int vllp_logstream_decode(const void *data, size_t len, uint32_t *seq,
                          int *level, int64_t *ms_ago,
                          const char **msg, size_t *msglen);
