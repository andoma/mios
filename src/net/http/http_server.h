#pragma once

#include <mios/mios.h>

struct http_request;
struct stream;

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
