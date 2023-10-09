#include "http_server.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/param.h>

#include "net/pbuf.h"

#include "http_parser.h"
#include "http_request.h"

#include <mios/service.h>
#include <mios/bumpalloc.h>
#include <mios/task.h>

STAILQ_HEAD(http_request_squeue, http_request);
TAILQ_HEAD(http_request_queue, http_request);

static struct http_request_squeue http_req_queue;

static mutex_t http_server_mutex = MUTEX_INITIALIZER("http");
static cond_t http_req_queue_cond = COND_INITIALIZER("rq");

static void http_stream_write(struct stream *s, const void *buf, size_t size);

typedef struct http_connection {

  stream_t s;

  struct http_parser hc_hp;

  socket_t *hc_sock;

  uint32_t hc_header_match;
  size_t hc_header_match_len;

  pbuf_t *hc_txbuf_head;
  pbuf_t *hc_txbuf_tail;

  http_request_t *hc_hr;

  struct http_request_queue hc_requests;

  cond_t hc_txbuf_cond;

  uint8_t hc_chunked_encoding;

} http_connection_t;

#define HEADER_HOST              0x1
#define HEADER_SEC_WEBSOCKET_KEY 0x2
#define HEADER_CONTENT_TYPE      0x4
#define HEADER_CONNECTION        0x8
#define HEADER_UPGRADE           0x10

#define HEADER_ALL (HEADER_HOST | \
                    HEADER_SEC_WEBSOCKET_KEY | \
                    HEADER_CONTENT_TYPE | \
                    HEADER_CONNECTION | \
                    HEADER_UPGRADE)

static int
http_message_begin(http_parser *p)
{
  http_connection_t *hc = p->data;
  hc->hc_header_match_len = 0;

  balloc_t *ba = balloc_create(4096);
  if(ba == NULL) {
    hc->hc_hr = NULL;
  } else {
    http_request_t *hr = balloc_alloc(ba, sizeof(http_request_t));
    memset(hr, 0, sizeof(http_request_t));
    hr->hr_ba = ba;
    hc->hc_hr = hr;
  }

  return 0;
}


static int
http_url(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = hc->hc_hr;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  if(balloc_append_data(hr->hr_ba, at, length, (void **)&hr->hr_url, NULL)) {
    hr->hr_header_err = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
  }
  return 0;
}


static void
match_header(http_connection_t *hc, char c,
             const char *name, size_t len,
             uint32_t mask)
{
  if(!(hc->hc_header_match & mask))
    return;

  if(hc->hc_header_match_len > len || name[hc->hc_header_match_len] != c) {
    hc->hc_header_match &= ~mask;
    return;
  }
}


static int
http_header_field(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = hc->hc_hr;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  if(hc->hc_header_match_len == 0) {
    hc->hc_header_match = HEADER_ALL;
  }

  for(size_t i = 0; i < length; i++) {
    char c = at[i];
    if(c >= 'A' && c <= 'Z')
      c += 32;
    match_header(hc, c, "host", strlen("host"),
                 HEADER_HOST);
    match_header(hc, c, "sec-websocket-key", strlen("sec-websocket-key"),
                 HEADER_SEC_WEBSOCKET_KEY);
    match_header(hc, c, "content-type", strlen("content-type"),
                 HEADER_CONTENT_TYPE);
    match_header(hc, c, "connection", strlen("connection"),
                 HEADER_CONNECTION);
    match_header(hc, c, "upgrade", strlen("upgrade"),
                 HEADER_UPGRADE);
    hc->hc_header_match_len++;
  }
  return 0;
}


static int
http_header_value(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = hc->hc_hr;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  hc->hc_header_match_len = 0;

  error_t err;
  switch(hc->hc_header_match) {
  case HEADER_HOST:
    err = balloc_append_data(hr->hr_ba, at, length,
                             (void **)&hr->hr_host, NULL);
    break;
  case HEADER_CONTENT_TYPE:
    err = balloc_append_data(hr->hr_ba, at, length,
                             (void **)&hr->hr_content_type, NULL);
    break;
  default:
    return 0;
  }
  if(err)
    hr->hr_header_err = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
  return 0;
}


static int
http_headers_complete(http_parser *p)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = hc->hc_hr;
  if(hr == NULL)
    return 0;

  return 0;
}


static int
http_body(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = hc->hc_hr;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  error_t err = balloc_append_data(hr->hr_ba, at, length,
                                   &hr->hr_body, &hr->hr_body_size);
  if(err)
    hr->hr_header_err = HTTP_STATUS_PAYLOAD_TOO_LARGE;

  return 0;
}



static pbuf_t *
make_http_response_simple(int code, int do_close, socket_t *s)
{
  pbuf_t *pb = pbuf_make(s->preferred_offset, 1);
  size_t len = snprintf(pbuf_data(pb, 0), PBUF_DATA_SIZE - pb->pb_offset,
                        "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n%s\r\n",
                        code, http_status_str(code),
                        do_close ? "Connection: close\r\n": "");

  pb->pb_pktlen += len;
  pb->pb_buflen += len;
  return pb;
}

static int
http_message_complete(http_parser *p)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = hc->hc_hr;

  mutex_lock(&http_server_mutex);

  if(hr == NULL) {

    http_request_t *last = TAILQ_LAST(&hc->hc_requests, http_request_queue);

    // We do some trickery here to correct deal with HTTP/1 pipelining
    // It's almost never used, but it's not really much extra work for us

    if(last == NULL) {
      // No requests are queued, we can respond directly
      hc->hc_txbuf_head = make_http_response_simple(503, 1, hc->hc_sock);
      hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_WAKEUP);
    } else {
      // We didn't manage to allocate a request struct,
      // Ask the request last in queue to also send a 503 when it completed
      last->hr_piggyback_503++;
    }
  } else {

    hr->hr_should_keep_alive = http_should_keep_alive(p);

    STAILQ_INSERT_TAIL(&http_req_queue, hr, hr_global_link);
    hr->hr_hc = hc;
    TAILQ_INSERT_TAIL(&hc->hc_requests, hr, hr_connection_link);
    cond_signal(&http_req_queue_cond);
    hc->hc_hr = NULL;
  }

  mutex_unlock(&http_server_mutex);
  return 0;
}


static const http_parser_settings parser_settings = {
  .on_message_begin    = http_message_begin,
  .on_url              = http_url,
  .on_header_field     = http_header_field,
  .on_header_value     = http_header_value,
  .on_headers_complete = http_headers_complete,
  .on_body             = http_body,
  .on_message_complete = http_message_complete,
};



static pbuf_t *
http_push(void *opaque, struct pbuf *pb0)
{
  http_connection_t *hc = opaque;
  for(pbuf_t *pb = pb0; pb != NULL; pb = pb->pb_next) {
    size_t r = http_parser_execute(&hc->hc_hp, &parser_settings,
                                   pbuf_cdata(pb, 0), pb->pb_buflen);
    if(hc->hc_hp.http_errno) {

      mutex_lock(&http_server_mutex);
      if(hc->hc_sock != NULL) {
        hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_CLOSE);
        hc->hc_sock = NULL;
      }
      mutex_unlock(&http_server_mutex);

      return pb0;
    }
    assert(r == pb->pb_buflen);
  }
  return pb0;
}


static int
http_may_push(void *opaque)
{
  return 1;
}

static const char hexdigit[16] = "0123456789abcdef";

static struct pbuf *
http_pull(void *opaque)
{
  http_connection_t *hc = opaque;

  mutex_lock(&http_server_mutex);
  pbuf_t *pb = hc->hc_txbuf_head;

  if(hc->hc_chunked_encoding) {
    size_t len = pb->pb_pktlen;
    pb = pbuf_prepend(pb, 8, 0, 0);
    assert(pb != NULL);
    char *hdr = pbuf_data(pb, 0);
    hdr[0] = '0';
    hdr[1] = '0';
    hdr[2] = '0';
    hdr[3] = hexdigit[(len >> 8) & 0xf];
    hdr[4] = hexdigit[(len >> 4) & 0xf];
    hdr[5] = hexdigit[(len >> 0) & 0xf];
    hdr[6] = '\r';
    hdr[7] = '\n';

    pbuf_t *tail = hc->hc_txbuf_tail;
    memcpy(tail->pb_data + tail->pb_offset + tail->pb_buflen, "\r\n", 2);
    tail->pb_buflen += 2;
    pb->pb_pktlen += 2;
  }

  hc->hc_txbuf_head = NULL;
  hc->hc_txbuf_tail = NULL;
  cond_signal(&hc->hc_txbuf_cond);
  mutex_unlock(&http_server_mutex);

  return pb;
}


static void
http_connection_maybe_free(http_connection_t *hc)
{
  if(TAILQ_FIRST(&hc->hc_requests) || hc->hc_sock)
    return;

  free(hc);
}

static void
http_close(void *opaque)
{
  http_connection_t *hc = opaque;

  if(hc->hc_hr != NULL) {
    free(hc->hc_hr->hr_ba);
    hc->hc_hr = NULL;
  }

  mutex_lock(&http_server_mutex);

  if(hc->hc_sock != NULL) {
    hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_CLOSE);
    hc->hc_sock = NULL;
  }

  http_connection_maybe_free(hc);
  mutex_unlock(&http_server_mutex);
}


static const socket_app_fn_t http_app_fn = {
  .push = http_push,
  .may_push = http_may_push,
  .pull = http_pull,
  .close = http_close
};


static error_t
http_open(socket_t *s)
{
  http_connection_t *hc = xalloc(sizeof(http_connection_t), 0, MEM_MAY_FAIL);
  if(hc == NULL)
    return ERR_NO_MEMORY;

  memset(hc, 0, sizeof(http_connection_t));
  TAILQ_INIT(&hc->hc_requests);
  hc->hc_sock = s;

  s->app = &http_app_fn;
  s->app_opaque = hc;

  http_parser_init(&hc->hc_hp, HTTP_REQUEST);
  hc->hc_hp.data = hc;
  hc->s.write = http_stream_write;
  cond_init(&hc->hc_txbuf_cond, "httpout");
  return 0;
}


SERVICE_DEF("http", 80, 0, SERVICE_TYPE_STREAM, http_open);



static int
match_route(const http_route_t *route, char *input, http_request_t *hr)
{
  size_t argc = 0;
  const char *argv[4];

  const char *r = route->hr_path;
  char *p = input;
  while(*p) {

    if(r[0] == '%') {
      // Wildcard
      if(argc == 4)
        return 404;

      argv[argc] = p;
      argc++;

      r++;

      while(*p != '/' && *p != 0)
        p++;
      continue;
    }
    if(*r != *p)
      return 404;
    r++;
    p++;
  }

  if(!(*r == 0 && *p == 0))
    return 404;

  for(size_t i = 0; i < argc; i++) {
    char *a = strchr(argv[i], '/');
    if(a)
      *a = 0;
  }

  return route->hr_callback(hr, argc, argv);
}


static void
request_process(http_request_t *hr)
{
  extern unsigned long _httproute_array_begin;
  extern unsigned long _httproute_array_end;

  char *url = hr->hr_url;

  int return_code = 404;

  mutex_unlock(&http_server_mutex);

  const http_route_t *r = (void *)&_httproute_array_begin;
  for(; r != (const void *)&_httproute_array_end; r++) {
    return_code = match_route(r, url + 1, hr);
    if(return_code != 404) {
      break;
    }
  }

  mutex_lock(&http_server_mutex);

  if(return_code == 0)
    return; // callback already sent the response

  http_connection_t *hc = hr->hr_hc;

  while(hc->hc_txbuf_head) {
    cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
  }

  if(hc->hc_sock) {
    hc->hc_txbuf_head = make_http_response_simple(return_code,
                                                  !hr->hr_should_keep_alive,
                                                  hc->hc_sock);
    hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_WAKEUP);
  }
}


__attribute__((noreturn))
static void *
http_req_thread(void *arg)
{
  mutex_lock(&http_server_mutex);
  while(1) {
    http_request_t *hr = STAILQ_FIRST(&http_req_queue);
    if(hr == NULL) {
      cond_wait(&http_req_queue_cond, &http_server_mutex);
      continue;
    }

    request_process(hr);

    http_connection_t *hc = hr->hr_hc;

    while(hr->hr_piggyback_503) {

      while(hc->hc_txbuf_head) {
        cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
      }

      if(hc->hc_sock == NULL)
        break;

      hc->hc_txbuf_head = make_http_response_simple(503, 1, hc->hc_sock);
      hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_WAKEUP);
      hr->hr_piggyback_503--;
    }

    TAILQ_REMOVE(&hc->hc_requests, hr, hr_connection_link);
    STAILQ_REMOVE_HEAD(&http_req_queue, hr_global_link);
    free(hr->hr_ba);
    http_connection_maybe_free(hc);
  }
}


static void __attribute__((constructor(300)))
http_init(void)
{
  STAILQ_INIT(&http_req_queue);
  thread_create(http_req_thread, NULL, 1024, "http",
                TASK_FPU | TASK_DETACHED, 8);
}


static void
http_stream_write(struct stream *s, const void *buf, size_t size)
{
  http_connection_t *hc = (http_connection_t *)s;

  mutex_lock(&http_server_mutex);

  socket_t *sk = hc->hc_sock;

  if(buf == NULL) {
    // Flush

    if(sk != NULL && hc->hc_txbuf_head != NULL) {
      sk->net->event(sk->net_opaque, SOCKET_EVENT_WAKEUP);
    }

  } else {

    while(size) {

      sk = hc->hc_sock;

      if(sk == NULL)
        break;

      if(hc->hc_txbuf_head == NULL) {

        size_t preferred_offset = sk->preferred_offset + 8;

        hc->hc_txbuf_head = pbuf_make(preferred_offset, 0);
        if(hc->hc_txbuf_head == NULL) {
          sk->net->event(sk->net_opaque, SOCKET_EVENT_WAKEUP);
          hc->hc_txbuf_head = pbuf_make(preferred_offset, 1);
        }
        hc->hc_txbuf_tail = hc->hc_txbuf_head;
      }

      size_t remain =
        sk->max_fragment_size - hc->hc_txbuf_head->pb_pktlen - 10;

      // Check if we need to allocate a new buffer for filling
      if(remain &&
         hc->hc_txbuf_tail->pb_buflen +
         hc->hc_txbuf_tail->pb_offset == PBUF_DATA_SIZE) {

        pbuf_t *pb = pbuf_make(0, 0);
        if(pb == NULL) {
          // Can't alloc, flush what we have
          remain = 0;
        } else {
          pb->pb_flags &= ~PBUF_SOP;
          hc->hc_txbuf_tail->pb_flags &= ~PBUF_EOP;
          hc->hc_txbuf_tail->pb_next = pb;
          hc->hc_txbuf_tail = pb;
        }
      }

      if(remain == 0) {
        sk->net->event(sk->net_opaque, SOCKET_EVENT_WAKEUP);
        cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
        continue;
      }
      pbuf_t *pb = hc->hc_txbuf_tail;
      size_t to_copy = MIN(MIN(size, remain),
                           PBUF_DATA_SIZE - (pb->pb_offset + pb->pb_buflen));

      memcpy(pb->pb_data + pb->pb_offset + pb->pb_buflen, buf, to_copy);
      pb->pb_buflen += to_copy;
      hc->hc_txbuf_head->pb_pktlen += to_copy;
      buf += to_copy;
      size -= to_copy;
    }
  }
  mutex_unlock(&http_server_mutex);
}



struct stream *
http_response_begin(struct http_request *hr,
                    int status_code,
                    const char *content_type)
{
  http_connection_t *hc = hr->hr_hc;

  mutex_lock(&http_server_mutex);

  while(hc->hc_txbuf_head) {
    cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
  }

  socket_t *sk = hc->hc_sock;
  if(sk != NULL) {

    pbuf_t *pb = pbuf_make(sk->preferred_offset, 1);
    size_t len = snprintf(pbuf_data(pb, 0), PBUF_DATA_SIZE - pb->pb_offset,
                          "HTTP/1.1 %d %s\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "Content-Type: %s\r\n"
                          "%s"
                          "\r\n",
                          status_code, http_status_str(status_code),
                          content_type,
                          !hr->hr_should_keep_alive ?
                          "Connection: close\r\n": "");

    pb->pb_pktlen += len;
    pb->pb_buflen += len;

    hc->hc_txbuf_head = pb;
    hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_WAKEUP);

    while(hc->hc_txbuf_head) {
      cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
    }
    hc->hc_chunked_encoding = 1;
  }

  mutex_unlock(&http_server_mutex);
  return &hc->s;
}

int
http_response_end(struct http_request *hr)
{
  http_connection_t *hc = hr->hr_hc;

  mutex_lock(&http_server_mutex);

  while(hc->hc_txbuf_head) {
    cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
  }
  hc->hc_chunked_encoding = 0;

  socket_t *sk = hc->hc_sock;
  if(sk != NULL) {

    pbuf_t *pb = pbuf_make(sk->preferred_offset, 1);
    memcpy(pbuf_data(pb, 0), "0\r\n\r\n", 5);

    pb->pb_pktlen += 5;
    pb->pb_buflen += 5;

    hc->hc_txbuf_head = pb;
    hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_WAKEUP);
  }

  mutex_unlock(&http_server_mutex);
  return 0;
}
