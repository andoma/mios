#pragma once

#include <stdint.h>


// Mirors the enum in include/mios/alert.h
typedef enum {
  ALERT_LEVEL_NOTICE,
  ALERT_LEVEL_WARNING,
  ALERT_LEVEL_ATTENTION,
  ALERT_LEVEL_ERROR,
} alert_level_t;

struct vllp;
typedef struct vllp_alertstream vllp_alertstream_t;

vllp_alertstream_t *vllp_alertstream_create(struct vllp *v, void *opaque,
                                            void (*mark)(void *opaque),
                                            void (*raise)(void *opaque,
                                                          const char *key,
                                                          alert_level_t level,
                                                          const char *msg),
                                            void (*sweep)(void *opaque));

void vllp_alertstream_destroy(vllp_alertstream_t *va);
