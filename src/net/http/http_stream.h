#pragma once

#include <mios/error.h>
#include <stdint.h>
#include <stddef.h>

#include "http_util.h"

struct stream;


typedef struct {
  uint16_t flags;
#define HTTP_DONT_FAIL_ON_ERROR 0x1   // HTTP STATUS >= 400 returns 0

  uint32_t tcp_socket_rx_buffer_size; // 0 = reasonable default

  uint64_t *content_length_ptr; /* if != NULL,
                                 * written before data flows (if known) */

} http_client_opts_t;

error_t http_get(const char *url, struct stream *output,
                 const http_client_opts_t *opts);
