#include "http_stream.h"

#include <assert.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/mios.h>
#include <mios/eventlog.h>
#include <mios/cli.h>

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>

#include "net/ipv4/tcp.h"
#include "net/pbuf.h"
#include "net/net.h"

#include "http_parser.h"

typedef struct {

  stream_t *hc_stream;

  error_t hc_error;
  uint16_t hc_flags;

  void *hc_opaque;
  const http_header_callback_t *hc_header_callbacks;
  size_t hc_num_header_callbacks;

  http_header_matcher_t hc_hhm;

} http_client_t;


static int
http_client_header_field(http_parser *p, const char *at, size_t length)
{
  http_client_t *hc = p->data;
  return http_match_header_field(&hc->hc_hhm, at, length,
                                 hc->hc_header_callbacks,
                                 hc->hc_num_header_callbacks);
}

static int
http_client_header_value(http_parser *p, const char *at, size_t length)
{
  http_client_t *hc = p->data;
  return http_match_header_value(&hc->hc_hhm, at, length,
                                 hc->hc_header_callbacks,
                                 hc->hc_num_header_callbacks,
                                 hc->hc_opaque);
}

static int
http_client_headers_complete(http_parser *p)
{
  http_client_t *hc = p->data;
  if(!(hc->hc_flags & HTTP_DONT_FAIL_ON_ERROR) && p->status_code >= 400) {
    hc->hc_error = ERR_OPERATION_FAILED;
    return 1;
  }
  return 0;
}

static int
http_client_body(http_parser *p, const char *at, size_t length)
{
  http_client_t *hc = p->data;
  if(hc->hc_error < 0)
    return 0;
  ssize_t r = stream_write(hc->hc_stream, at, length, 0);
  if(r == 0) {
    hc->hc_error = 0;
    return 1;
  }
  if(r < 0)
    hc->hc_error = r;
  return 0;
}


static int
http_client_message_complete(http_parser *p)
{
  http_client_t *hc = p->data;
  ssize_t r = stream_write(hc->hc_stream, NULL, 0, 0);
  if(r < 0)
    hc->hc_error = r;
  if(hc->hc_error > 0)
    hc->hc_error = 0;
  return 0;
}


__attribute__((unused))
static const http_parser_settings client_parser = {
  .on_header_field     = http_client_header_field,
  .on_header_value     = http_client_header_value,
  .on_headers_complete = http_client_headers_complete,
  .on_body             = http_client_body,
  .on_message_complete = http_client_message_complete,
};




error_t
http_get(const char *url, stream_t *output,
         const http_client_opts_t *opts)
{
  struct http_parser_url up;
  if(http_parser_parse_url(url, strlen(url), 0, &up))
    return ERR_MALFORMED;

  if((up.field_set & (UF_SCHEMA | UF_HOST | UF_PATH)) !=
     (UF_SCHEMA | UF_HOST | UF_PATH))
    return ERR_INVALID_ADDRESS;

  http_client_t *hc = xalloc(sizeof(http_client_t), 0,
                             MEM_MAY_FAIL | MEM_CLEAR);
  if(hc == NULL)
    return ERR_NO_MEMORY;

  hc->hc_stream = output;

  int rxbufsize = 4096;

  if(opts) {
    hc->hc_flags = opts->flags;
    if(opts->tcp_socket_rx_buffer_size)
      rxbufsize = opts->tcp_socket_rx_buffer_size;
  }

  stream_t *sk = tcp_create_socket("httpclient", 2048, rxbufsize);
  if(sk == NULL) {
    free(hc);
    return ERR_NO_MEMORY;
  }

  int port = up.field_set & (1 << UF_PORT) ? up.port : 80;
  uint32_t addr = inet_addr(url + up.field_data[UF_HOST].off);
  tcp_connect(sk, addr, port);

  stprintf(sk, "GET ");
  stream_write(sk, url + up.field_data[UF_PATH].off,
               up.field_data[UF_PATH].len, 0);
  stprintf(sk, " HTTP/1.1\r\nConnection: close\r\n\r\n");
  stream_flush(sk);

  struct http_parser hp;
  http_parser_init(&hp, HTTP_RESPONSE);
  hp.data = hc;

  hc->hc_error = 1;

  while(hc->hc_error == 1) {

    void *buf;

    ssize_t bytes = stream_peek(sk, &buf, 1);
    if(bytes < 0) {
      hc->hc_error = bytes;
      break;
    }

    int r = http_parser_execute(&hp, &client_parser, buf, bytes);
    if(r < 0) {
      if(hc->hc_error == 1)
        hc->hc_error = ERR_BAD_STATE;
      break;
    }

    stream_drop(sk, r);
  }

  stream_close(sk);

  error_t rval = hc->hc_error;
  free(hc);
  return rval;
}


static error_t
cmd_curl(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;
  return http_get(argv[1], cli->cl_stream, NULL);
}

CLI_CMD_DEF("curl", cmd_curl);
