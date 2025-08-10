#pragma once

#include <stdint.h>

struct vllp;
typedef struct vllp_logstream vllp_logstream_t;

struct vllp_logstream *vllp_logstream_create(struct vllp *v, void *opaque,
                                             void (*cb)(void *opaque,
                                                        int level, uint32_t sequence,
                                                        int64_t ms_ago, const char *msg));

void vllp_logstream_destroy(struct vllp_logstream *vl);
