#pragma once

#include <stdint.h>
#include <mios/mios.h>
#include <sys/queue.h>

struct stream;

typedef struct http_request {

  STAILQ_ENTRY(http_request) hr_global_link;

  TAILQ_ENTRY(http_request) hr_connection_link;

  struct http_connection *hr_hc;

  struct balloc *hr_ba;

  // Headers
  char *hr_url;
  char *hr_host;
  char *hr_content_type;
  char *hr_upgrade;
  char *hr_connection;
  char *hr_wskey;

  void *hr_body;
  size_t hr_body_size;

  uint16_t hr_header_err;
  uint16_t hr_piggyback_503;
  uint8_t hr_should_keep_alive;

} http_request_t;

struct stream *http_response_begin(struct http_request *hr,
                                   int status_code,
                                   const char *content_type);

int http_response_end(struct http_request *hr);

typedef struct http_route {

  const char *hr_path;

  int (*hr_callback)(struct http_request *hr, int argc, const char **argv);

} http_route_t;

#define HTTP_ROUTE_DEF(path, cb) \
  static const http_route_t MIOS_JOIN(rpc, __LINE__) __attribute__ ((used, section("httproute"))) = { path, cb};
