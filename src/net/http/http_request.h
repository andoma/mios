#pragma once

#include <sys/queue.h>

typedef struct http_request {

  STAILQ_ENTRY(http_request) hr_global_link;

  TAILQ_ENTRY(http_request) hr_connection_link;

  struct http_connection *hr_hc;

  struct balloc *hr_ba;

  char *hr_url;

  char *hr_host;
  char *hr_content_type;

  void *hr_body;
  size_t hr_body_size;

  uint16_t hr_header_err;
  uint16_t hr_piggyback_503;
  uint8_t hr_should_keep_alive;
  
} http_request_t;
