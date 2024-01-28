#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *name;
  int (*cb)(void *opaque, const char *str, size_t len);
} http_header_callback_t;

typedef struct {

  uint16_t hhm_mask;
  uint8_t hhm_len;

} http_header_matcher_t;

int http_match_header_field(http_header_matcher_t *hhm,
                            const char *str, size_t len,
                            const http_header_callback_t *callbacks,
                            size_t num_callbacks);

int http_match_header_value(http_header_matcher_t *hhm,
                            const char *str, size_t len,
                            const http_header_callback_t *callbacks,
                            size_t num_callbacks, void *opaque);
