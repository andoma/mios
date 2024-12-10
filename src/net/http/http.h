#pragma once

#include <stdint.h>
#include <mios/mios.h>
#include <mios/bumpalloc.h>
#include <sys/queue.h>

#include "http_util.h"

struct iovec;
struct stream;

typedef struct http_connection http_connection_t;


typedef struct http_request {

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
  uint8_t hr_upgrade_to_websocket;

  http_header_matcher_t hr_header_matcher;

  http_connection_t *hr_hc;

  balloc_t hr_bumpalloc;

} http_request_t;

struct stream *http_response_begin(struct http_request *hr,
                                   int status_code,
                                   const char *content_type);

int http_request_accept_websocket(http_request_t *hr,
                                  int (*cb)(void *opaque,
                                            int opcode,
                                            void *data,
                                            size_t size,
                                            http_connection_t *hc,
                                            balloc_t *ba),
                                  void *opaque,
                                  http_connection_t **hcp);


struct stream *http_websocket_output_begin(http_connection_t *hc,
                                           int opcode);

ssize_t http_websocket_sendv(http_connection_t *hc, int opcode,
                             struct iovec *iov, size_t iovcnt,
                             int flags);

void http_connection_retain(http_connection_t *hc);

void http_connection_release(http_connection_t *hc);

// name must be compile time constant
http_connection_t *http_websocket_create(int (*cb)(void *opaque,
                                                   int opcode,
                                                   void *data,
                                                   size_t size,
                                                   http_connection_t *hc,
                                                   balloc_t *ba),
                                         void *opaque,
                                         const char *name);

__attribute__((warn_unused_result))
error_t http_websocket_start(http_connection_t *hc, uint32_t addr,
                             uint16_t port, const char *path,
                             const char *protocol);


void http_websocket_close(http_connection_t *hc, uint16_t status_code,
                          const char *message);

typedef struct http_route {

  const char *hr_path;

  int (*hr_callback)(struct http_request *hr, int argc, const char **argv);

} http_route_t;

#define HTTP_ROUTE_DEF(path, cb) \
  static const http_route_t MIOS_JOIN(rpc, __LINE__) __attribute__ ((used, section("httproute"))) = { path, cb};
