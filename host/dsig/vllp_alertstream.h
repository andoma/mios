#pragma once

#include <stdint.h>

struct vllp;
typedef struct vllp_alertstream vllp_alertstream_t;

vllp_alertstream_t *vllp_alertstream_create(struct vllp *v, void *opaque,
                                            void (*mark)(void *opaque),
                                            void (*raise)(void *opaque,
                                                          const char *key,
                                                          int level,
                                                          const char *msg),
                                            void (*sweep)(void *opaque));

void vllp_alertstream_destroy(vllp_alertstream_t *va);
