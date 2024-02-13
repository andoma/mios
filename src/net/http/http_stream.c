#include "http_stream.h"
#include <assert.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/mios.h>
#include <mios/fifo.h>

#include <socket.h>
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

  void *hc_opaque;

  pbuf_t *hc_txbuf;

  pbuf_t *hc_rxbuf;

  mutex_t hc_mutex;
  cond_t hc_rxcond;

  error_t hc_error;
  uint16_t hc_flags;
  uint8_t hc_closed;

  const http_header_callback_t *hc_header_callbacks;
  size_t hc_num_header_callbacks;

  http_header_matcher_t hc_hhm;

} http_client_t;


static struct pbuf *
http_client_pull(void *opaque)
{
  http_client_t *hc = opaque;
  pbuf_t *pb = hc->hc_txbuf;
  hc->hc_txbuf = NULL;
  return pb;
}

static void
http_client_close(void *opaque, const char *reason)
{
  http_client_t *hc = opaque;
  mutex_lock(&hc->hc_mutex);
  hc->hc_closed = 1;
  cond_signal(&hc->hc_rxcond);
  mutex_unlock(&hc->hc_mutex);
}



static uint32_t
http_client_push(void *opaque, struct pbuf *pb)
{
  http_client_t *hc = opaque;
  mutex_lock(&hc->hc_mutex);
  if(hc->hc_rxbuf == NULL) {
    hc->hc_rxbuf = pb;
    cond_signal(&hc->hc_rxcond);
  } else {
    pbuf_t *tail = hc->hc_rxbuf;
    while(tail->pb_next) {
      tail = tail->pb_next;
    }
    tail->pb_next = pb;
  }
  mutex_unlock(&hc->hc_mutex);
  return 0;
}

static int
http_client_may_push(void *opaque)
{
  return 1;
}

static const socket_app_fn_t http_client_sock_fn = {
  .push = http_client_push,
  .may_push = http_client_may_push,
  .pull = http_client_pull,
  .close = http_client_close
};



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
  if(hc->hc_flags & HTTP_FAIL_ON_ERROR && p->status_code >= 400) {
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
  hc->hc_stream->write(hc->hc_stream, at, length, 0);
  return 0;
}


static int
http_client_message_complete(http_parser *p)
{
  http_client_t *hc = p->data;
  hc->hc_stream->write(hc->hc_stream, NULL, 0, 0);
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


static void
pbuf_push_buf(pbuf_t *pb, const void *data, size_t len)
{
  memcpy(pbuf_append(pb, len), data, len);
}

static void
pbuf_push_str(pbuf_t *pb, const char *str)
{
  pbuf_push_buf(pb, str, strlen(str));
}

error_t
http_get(const char *url, stream_t *output, uint16_t flags,
         void *opaque,
         const http_header_callback_t *header_callbacks,
         size_t num_header_callbacks)
{
  struct http_parser_url up;
  if(http_parser_parse_url(url, strlen(url), 0, &up))
    return ERR_MALFORMED;

  if((up.field_set & (UF_SCHEMA | UF_HOST | UF_PATH)) !=
     (UF_SCHEMA | UF_HOST | UF_PATH))
    return ERR_INVALID_ADDRESS;

  http_client_t *hc = xalloc(sizeof(http_client_t), 0, MEM_MAY_FAIL);
  if(hc == NULL)
    return ERR_NO_MEMORY;
  memset(hc, 0, sizeof(http_client_t));

  hc->hc_flags = flags;
  hc->hc_stream = output;
  hc->hc_header_callbacks = header_callbacks;
  hc->hc_num_header_callbacks = num_header_callbacks;
  hc->hc_opaque = opaque;
  mutex_init(&hc->hc_mutex, "httpclient");
  cond_init(&hc->hc_rxcond, "httpclient");

  socket_t *sk = tcp_create_socket("httpclient");
  if(sk == NULL) {
    free(hc);
    return ERR_NO_MEMORY;
  }

  int port = up.field_set & UF_PORT ? up.port : 80;

  sk->app = &http_client_sock_fn;
  sk->app_opaque = hc;

  pbuf_t *pb = pbuf_make(sk->preferred_offset, 1);

  pbuf_push_str(pb, "GET ");
  pbuf_push_buf(pb, url + up.field_data[UF_PATH].off,
                up.field_data[UF_PATH].len);
  pbuf_push_str(pb, " HTTP/1.1\r\nConnection: close\r\n\r\n");

  hc->hc_txbuf = pb;

  uint32_t addr = inet_addr(url + up.field_data[UF_HOST].off);
  tcp_connect(sk, addr, port);

  struct http_parser hp;
  http_parser_init(&hp, HTTP_RESPONSE);
  hp.data = hc;

  mutex_lock(&hc->hc_mutex);

  hc->hc_error = 1;

  while(hc->hc_error == 1) {

    if(hc->hc_rxbuf == NULL) {
      if(hc->hc_closed)
        break;

      cond_wait(&hc->hc_rxcond, &hc->hc_mutex);
      continue;
    }

    pbuf_t *pb0 = hc->hc_rxbuf;
    hc->hc_rxbuf = NULL;
    mutex_unlock(&hc->hc_mutex);

    sk->net->event(sk->net_opaque, SOCKET_EVENT_PUSH);

    for(pbuf_t *pb = pb0 ; pb != NULL; pb = pb->pb_next) {
      size_t offset = 0;

      while(offset != pb->pb_buflen) {
        int r = http_parser_execute(&hp, &client_parser,
                                    pbuf_cdata(pb, offset),
                                    pb->pb_buflen - offset);
        assert(r > 0);
        if(r < 0) {
          hc->hc_error = ERR_BAD_STATE;
          break;
        }
        offset += r;
      }
    }
    pbuf_free(pb0);
    mutex_lock(&hc->hc_mutex);
  }

  sk->net->event(sk->net_opaque, SOCKET_EVENT_CLOSE);

  while(!hc->hc_closed) {
    cond_wait(&hc->hc_rxcond, &hc->hc_mutex);
  }

  mutex_unlock(&hc->hc_mutex);

  error_t rval = hc->hc_error;
  if(rval > 0)
    rval = ERR_NOT_CONNECTED;

  free(hc);
  return rval;
}



#include <mios/cli.h>

static error_t
cmd_curl(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;
  return http_get(argv[1], cli->cl_stream, HTTP_FAIL_ON_ERROR, NULL, NULL, 0);
}


CLI_CMD_DEF("curl", cmd_curl);
