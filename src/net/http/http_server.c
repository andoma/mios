#include "http_server.h"

#include <assert.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/param.h>

#include "net/pbuf.h"
#include "net/net_task.h"

#include "http_parser.h"
#include "websocket.h"

#include "lib/crypto/sha1.h"
#include "util/base64.h"

#include <mios/service.h>
#include <mios/bumpalloc.h>
#include <mios/task.h>
#include <mios/bytestream.h>
#include <mios/timer.h>

STAILQ_HEAD(http_server_task_squeue, http_server_task);
TAILQ_HEAD(http_server_task_queue, http_server_task);

static struct http_server_task_squeue http_task_queue;

static mutex_t http_server_mutex = MUTEX_INITIALIZER("http");
static cond_t http_task_queue_cond = COND_INITIALIZER("rq");

static void http_stream_write(struct stream *s, const void *buf, size_t size);

typedef struct http_connection {

  stream_t s;

  union {
    struct http_parser hc_hp;
    struct websocket_parser hc_wp;
  };

  socket_t *hc_sock;

  pbuf_t *hc_txbuf_head;
  pbuf_t *hc_txbuf_tail;
  cond_t hc_txbuf_cond;

  http_server_task_t *hc_task;
  http_server_task_t *hc_ctrl_task;
  struct http_server_task_queue hc_tasks;

  uint8_t hc_chunked_encoding;
  uint8_t hc_websocket_mode;
  uint8_t hc_net_closed;
  uint8_t hc_ping_counter;

  timer_t hc_timer;

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



static void
http_timer_arm(http_connection_t *hc, int seconds)
{
  net_timer_arm(&hc->hc_timer, clock_get() + seconds * 1000000);
}


static int
http_message_begin(http_parser *p)
{
  const size_t alloc_size = 4096;

  http_request_t *hr = xalloc(alloc_size, 0, MEM_MAY_FAIL);
  if(hr == NULL) {
    return 0;
  }

  memset(hr, 0, sizeof(http_request_t));
  hr->hr_bumpalloc.capacity = alloc_size - sizeof(http_request_t);

  http_connection_t *hc = p->data;
  hc->hc_task = &hr->hr_hst;
  return 0;
}


static int
http_url(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_task;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  if(!balloc_append_data(&hr->hr_bumpalloc, at, length,
                         (void **)&hr->hr_url, NULL)) {
    hr->hr_header_err = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
  }
  return 0;
}


static void
match_header(http_request_t *hr, char c, const char *name, size_t len,
             uint16_t mask)
{
  if(!(hr->hr_header_match & mask))
    return;

  if(hr->hr_header_match_len > len || name[hr->hr_header_match_len] != c) {
    hr->hr_header_match &= ~mask;
    return;
  }
}


static int
http_header_field(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_task;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  if(hr->hr_header_match_len == 0) {
    hr->hr_header_match = HEADER_ALL;
  }

  if(hr->hr_header_match_len == 255)
    return 0;

  for(size_t i = 0; i < length; i++) {
    char c = at[i];
    if(c >= 'A' && c <= 'Z')
      c += 32;
    match_header(hr, c, "host", strlen("host"),
                 HEADER_HOST);
    match_header(hr, c, "sec-websocket-key", strlen("sec-websocket-key"),
                 HEADER_SEC_WEBSOCKET_KEY);
    match_header(hr, c, "content-type", strlen("content-type"),
                 HEADER_CONTENT_TYPE);
    match_header(hr, c, "connection", strlen("connection"),
                 HEADER_CONNECTION);
    match_header(hr, c, "upgrade", strlen("upgrade"),
                 HEADER_UPGRADE);
    hr->hr_header_match_len++;
  }
  return 0;
}


static int
http_header_value(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_task;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  hr->hr_header_match_len = 0;

  void *dst;
  switch(hr->hr_header_match) {
  case HEADER_HOST:
    dst = balloc_append_data(&hr->hr_bumpalloc, at, length,
                             (void **)&hr->hr_host, NULL);
    break;
  case HEADER_SEC_WEBSOCKET_KEY:
    dst = balloc_append_data(&hr->hr_bumpalloc, at, length,
                             (void **)&hr->hr_wskey, NULL);
    break;
  case HEADER_CONTENT_TYPE:
    dst = balloc_append_data(&hr->hr_bumpalloc, at, length,
                             (void **)&hr->hr_content_type, NULL);
    break;
  case HEADER_UPGRADE:
    dst = balloc_append_data(&hr->hr_bumpalloc, at, length,
                             (void **)&hr->hr_upgrade, NULL);
    break;
  case HEADER_CONNECTION:
    dst = balloc_append_data(&hr->hr_bumpalloc, at, length,
                             (void **)&hr->hr_connection, NULL);
    break;
  default:
    return 0;
  }
  if(dst == NULL)
    hr->hr_header_err = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
  return 0;
}


static int
http_headers_complete(http_parser *p)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_task;
  if(hr == NULL)
    return 0;

  if(hr->hr_connection && !strcasecmp(hr->hr_connection, "upgrade") &&
     hr->hr_upgrade && !strcasecmp(hr->hr_upgrade, "websocket")) {
    hc->hc_websocket_mode = 1;
    http_timer_arm(hc, 5);
    return 2;
  }

  http_timer_arm(hc, 20);
  return 0;
}


static int
http_body(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_task;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  if(!balloc_append_data(&hr->hr_bumpalloc, at, length,
                         &hr->hr_body, &hr->hr_body_size)) {
    hr->hr_header_err = HTTP_STATUS_PAYLOAD_TOO_LARGE;
  }

  return 0;
}

static pbuf_t *
make_response(socket_t *sk, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);

  pbuf_t *pb = pbuf_make(sk->preferred_offset, 1);
  size_t len = vsnprintf(pbuf_data(pb, 0), PBUF_DATA_SIZE - pb->pb_offset,
                         fmt, ap);
  va_end(ap);
  pb->pb_pktlen += len;
  pb->pb_buflen += len;
  return pb;
}



static void
send_response_simple(http_connection_t *hc, int code, int do_close)
{
  socket_t *sk = hc->hc_sock;
  if(sk == NULL)
    return;

  hc->hc_txbuf_head =
    make_response(sk, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n%s\r\n",
                  code, http_status_str(code),
                  do_close ? "Connection: close\r\n": "");

  sk->net->event(sk->net_opaque, SOCKET_EVENT_WAKEUP);
}


static void
http_task_enqueue(http_server_task_t *hst, http_connection_t *hc,
                  http_server_task_type_t type)
{
  hst->hst_type = type;
  STAILQ_INSERT_TAIL(&http_task_queue, hst, hst_global_link);
  hst->hst_hc = hc;
  TAILQ_INSERT_TAIL(&hc->hc_tasks, hst, hst_connection_link);
  cond_signal(&http_task_queue_cond);
}

static int
http_message_complete(http_parser *p)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_task;

  mutex_lock(&http_server_mutex);

  if(hr == NULL) {

    http_server_task_t *last =
      TAILQ_LAST(&hc->hc_tasks, http_server_task_queue);

    // We do some trickery here to correct deal with HTTP/1 pipelining
    // It's almost never used, but it's not really much extra work for us
    if(last == NULL) {
      // No requests are queued, we can respond directly
      send_response_simple(hc, 503, 1);
    } else {
      // We didn't manage to allocate a request struct,
      // Ask the request last in queue to also send a 503 when it completed
      http_request_t *hr = (http_request_t *)last;
      hr->hr_piggyback_503++;
    }
  } else {

    hr->hr_should_keep_alive = http_should_keep_alive(p);

    http_task_enqueue(&hr->hr_hst, hc, HST_HTTP_REQ);
    hc->hc_task = NULL;
  }

  mutex_unlock(&http_server_mutex);
  http_timer_arm(hc, 5);
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


static http_server_wsp_t *
hsw_acquire(http_server_task_t **p, size_t capacity, uint8_t opcode)
{
  if(*p)
    return (http_server_wsp_t *)*p;

  http_server_wsp_t *hsw = xalloc(sizeof(http_server_wsp_t) + capacity,
                                  0, MEM_MAY_FAIL);
  if(hsw == NULL)
    return NULL;

  *p = &hsw->hsw_hst;
  memset(hsw, 0, sizeof(http_server_wsp_t));
  hsw->hsw_hst.hst_opcode = opcode;
  hsw->hsw_capacity = capacity;
  return hsw;
}


static int
websocket_parser_execute(http_connection_t *hc,
                         const uint8_t *pkt, size_t total_len)
{
  websocket_parser_t *wp = &hc->hc_wp;

  size_t pkt_len = total_len;
  size_t consumed = 0;
  uint8_t rewind_to = wp->wp_header_len;

  while(1) {
    int req_hdr_len = 2;
    if(wp->wp_header_len >= 2) {
      int frag_len = wp->wp_header[1] & 0x7f;
      if(frag_len == 126)
        req_hdr_len = 4;
      else if(frag_len == 127)
        req_hdr_len = 10;

      if(wp->wp_header[1] & 0x80)
        req_hdr_len += 4; // mask
    }

    if(wp->wp_header_len == req_hdr_len)
      break;

    const size_t to_copy = MIN(req_hdr_len - wp->wp_header_len, pkt_len);
    if(to_copy == 0)
      return consumed;
    memcpy(wp->wp_header + wp->wp_header_len, pkt, to_copy);
    pkt += to_copy;
    pkt_len -= to_copy;
    consumed += to_copy;
    wp->wp_header_len += to_copy;
  }

  int64_t frag_len = wp->wp_header[1] & 0x7f;
  int mask_off = 2;

  if(frag_len == 126) {
    frag_len = wp->wp_header[2] << 8 | wp->wp_header[3];
    mask_off = 4;
  } else if(frag_len == 127) {
    frag_len = rd64_be(wp->wp_header + 2);
    mask_off = 10;
  }

  if(frag_len > 1024 * 1024)
    return -1;

  const int opcode = wp->wp_header[0] & 0xf;

  http_server_wsp_t *hsw;

  if(opcode & 0x8) {

    if(!(wp->wp_header[0] & 0x80) || frag_len > 125)
      return -1;

    // We special case PONG replies (just discard data) and reset
    // our ping counter to reset timeout countdown of doom
    if(opcode == WS_OPCODE_PONG) {
      hc->hc_ping_counter = 0;
      size_t to_discard = MIN(frag_len - wp->wp_fragment_used, pkt_len);
      wp->wp_fragment_used += to_discard;
      consumed += to_discard;

      if(wp->wp_fragment_used == frag_len) {
        wp->wp_header_len = 0;
        wp->wp_fragment_used = 0;
      }
      return consumed;
    }

    // Control frame
    // These can appear within fragmented non-control-frames
    // Also control frames themselves may not be fragmented
    hsw = hsw_acquire(&hc->hc_ctrl_task, frag_len, opcode);
  } else {
    hsw = hsw_acquire(&hc->hc_task, 4096, opcode);
  }

  if(hsw == NULL) {
    wp->wp_header_len = rewind_to;
    return 0;
  }
  const size_t to_copy = MIN(frag_len - wp->wp_fragment_used, pkt_len);
  if(hsw->hsw_used + to_copy > hsw->hsw_capacity) {
    return -1;
  }

  uint8_t *dst = hsw->hsw_data + hsw->hsw_used;
  memcpy(dst, pkt, to_copy);
  if(wp->wp_header[1] & 0x80) {
    const uint8_t *mask = wp->wp_header + mask_off;
    for(size_t i = 0; i < to_copy; i++) {
      dst[i] ^= mask[i&3];
    }
  }

  consumed += to_copy;
  wp->wp_fragment_used += to_copy;
  hsw->hsw_used += to_copy;

  if(wp->wp_fragment_used == frag_len) {

    if(wp->wp_header[0] & 0x80) {
      mutex_lock(&http_server_mutex);
      http_task_enqueue(&hsw->hsw_hst, hc, HST_WEBSOCKET_PACKET);
      mutex_unlock(&http_server_mutex);

      if(opcode & 0x8)
        hc->hc_ctrl_task = NULL;
      else
        hc->hc_task = NULL;
    }
    wp->wp_header_len = 0;
    wp->wp_fragment_used = 0;
  }

  // Return -1 to close
  return opcode == WS_OPCODE_CLOSE ? -1 : consumed;
}


static void
http_close_locked(http_connection_t *hc)
{
  if(hc->hc_sock != NULL) {
    hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_CLOSE);
    hc->hc_sock = NULL;
  }
}


static size_t
http_push_error(http_connection_t *hc, struct pbuf *pb0)
{
  mutex_lock(&http_server_mutex);
  http_close_locked(hc);
  mutex_unlock(&http_server_mutex);
  return pb0->pb_pktlen;
}

static size_t
http_push_partial(void *opaque, struct pbuf * const pb0)
{
  http_connection_t *hc = opaque;

  size_t consumed = 0;

  for(pbuf_t *pb = pb0 ; pb != NULL; pb = pb->pb_next) {
    size_t offset = 0;

    while(offset != pb->pb_buflen) {

      int r;
      if(hc->hc_websocket_mode) {
        websocket_parser_t *wp = &hc->hc_wp;

        if(hc->hc_websocket_mode == 1) {
          memset(wp, 0, sizeof(websocket_parser_t));
          hc->hc_websocket_mode++;
        }

        size_t offered = pb->pb_buflen - offset;
        r = websocket_parser_execute(hc, pbuf_cdata(pb, offset), offered);
        if(r < 0) {
          return http_push_error(hc, pb0);
        }

        if(r != offered)
          return consumed + r;

      } else {

        r = http_parser_execute(&hc->hc_hp, &parser_settings,
                                pbuf_cdata(pb, offset),
                                pb->pb_buflen - offset);

        if(hc->hc_hp.http_errno) {
          return http_push_error(hc, pb0);
        }
      }
      offset += r;
      consumed += r;
    }
  }
  return consumed;
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
  if(TAILQ_FIRST(&hc->hc_tasks) || !hc->hc_net_closed)
    return;

  free(hc);
}

static void
http_close(void *opaque)
{
  http_connection_t *hc = opaque;

  if(hc->hc_task != NULL) {
    free(hc->hc_task);
    hc->hc_task = NULL;
  }

  if(hc->hc_ctrl_task != NULL) {
    free(hc->hc_ctrl_task);
    hc->hc_ctrl_task = NULL;
  }

  timer_disarm(&hc->hc_timer);

  mutex_lock(&http_server_mutex);
  http_close_locked(hc);

  hc->hc_net_closed = 1;
  http_connection_maybe_free(hc);
  mutex_unlock(&http_server_mutex);
}


static const socket_app_fn_t http_app_fn = {
  .push_partial = http_push_partial,
  .pull = http_pull,
  .close = http_close
};

static void
http_timer_locked(http_connection_t *hc)
{
  if(hc->hc_websocket_mode) {

    socket_t *sk = hc->hc_sock;
    if(sk == NULL)
      return;

    if(hc->hc_ping_counter < 3) {

      http_timer_arm(hc, 5);

      if(hc->hc_txbuf_head != NULL)
        return;

      pbuf_t *pb = pbuf_make(sk->preferred_offset, 0);
      if(pb == NULL)
        return;
      uint8_t *pkt = pbuf_append(pb, 4);
      pkt[0] = 0x80 | WS_OPCODE_PING;
      pkt[1] = 2;
      pkt[2] = 0xde;
      pkt[3] = 0xad;
      hc->hc_txbuf_head = pb;

      sk->net->event(sk->net_opaque, SOCKET_EVENT_WAKEUP);
      hc->hc_ping_counter++;
      return;
    }
  }
  http_close_locked(hc);
}

static void
http_timer_cb(void *opaque, uint64_t now)
{
  http_connection_t *hc = opaque;
  mutex_lock(&http_server_mutex);
  http_timer_locked(hc);
  mutex_unlock(&http_server_mutex);
}


static error_t
http_open(socket_t *s)
{
  http_connection_t *hc = xalloc(sizeof(http_connection_t), 0, MEM_MAY_FAIL);
  if(hc == NULL)
    return ERR_NO_MEMORY;
  memset(hc, 0, sizeof(http_connection_t));

  TAILQ_INIT(&hc->hc_tasks);
  hc->hc_sock = s;

  s->app = &http_app_fn;
  s->app_opaque = hc;

  http_parser_init(&hc->hc_hp, HTTP_REQUEST);
  hc->hc_hp.data = hc;
  hc->s.write = http_stream_write;
  cond_init(&hc->hc_txbuf_cond, "httpout");

  hc->hc_timer.t_cb = http_timer_cb;
  hc->hc_timer.t_opaque = hc;
  hc->hc_timer.t_name = "http";

  http_timer_arm(hc, 5);
  return 0;
}


SERVICE_DEF("http", 80, 0, SERVICE_TYPE_STREAM, http_open);



static int
match_route(http_request_t *hr, char *path, const http_route_t *route)
{
  size_t argc = 0;
  const char *argv[4];

  const char *r = route->hr_path;

  while(*path) {

    if(r[0] == '%') {
      // Wildcard
      if(argc == 4)
        return HTTP_STATUS_NOT_FOUND;

      argv[argc] = path;
      argc++;

      r++;

      while(*path != '/' && *path != 0)
        path++;
      continue;
    }
    if(*r != *path)
      return HTTP_STATUS_NOT_FOUND;
    r++;
    path++;
  }

  if(!(*r == 0 && *path == 0))
    return HTTP_STATUS_NOT_FOUND;

  for(size_t i = 0; i < argc; i++) {
    char *a = strchr(argv[i], '/');
    if(a)
      *a = 0;
  }

  return route->hr_callback(hr, argc, argv);
}

static int
find_route(http_request_t *hr, char *path)
{
  if(*path != '/')
    return HTTP_STATUS_BAD_REQUEST;

  path++;
  extern unsigned long _httproute_array_begin;
  extern unsigned long _httproute_array_end;

  const http_route_t *r = (void *)&_httproute_array_begin;
  for(; r != (const void *)&_httproute_array_end; r++) {
    int rc = match_route(hr, path, r);
    if(rc != HTTP_STATUS_NOT_FOUND) {
      return rc;
    }
  }
  return HTTP_STATUS_NOT_FOUND;
}


static void
http_websocket_send_locked(http_connection_t *hc, uint8_t opcode,
                           const void *data, size_t len)
{
  while(len) {
    while(hc->hc_txbuf_head) {
      cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
    }

    socket_t *sk = hc->hc_sock;
    if(sk == NULL)
      break;

    pbuf_t *pb = pbuf_make(sk->preferred_offset, 1);
    uint8_t *hdr;
    int hdrlen;
    if(len < 126) {
      hdr = pbuf_append(pb, 2);
      hdrlen = 2;
      hdr[1] = len;
    } else {
      hdr = pbuf_append(pb, 4);
      hdrlen = 4;
      hdr[1] = 126;
      hdr[2] = len >> 8;
      hdr[3] = len;
    }
    hdr[0] = opcode;
    opcode = 0;

    size_t to_copy = MIN(len, PBUF_DATA_SIZE - pb->pb_offset);
    memcpy(pbuf_data(pb, hdrlen), data, to_copy);
    pb->pb_pktlen += to_copy;
    pb->pb_buflen += to_copy;

    data += to_copy;
    len -= to_copy;

    if(len == 0)
      hdr[0] |= 0x80;

    hc->hc_txbuf_head = pb;
    sk->net->event(sk->net_opaque, SOCKET_EVENT_WAKEUP);
  }
}


static void
http_process_websocket_packet(http_server_wsp_t *hsw)
{
  http_connection_t *hc = hsw->hsw_hst.hst_hc;
  switch(hsw->hsw_hst.hst_opcode) {
  case WS_OPCODE_PING:
    http_websocket_send_locked(hc, WS_OPCODE_PONG,
                               hsw->hsw_data, hsw->hsw_used);
    break;

  case WS_OPCODE_CLOSE:
    break;

  default:
    printf("Got websocket packet opcode:%d\n", hsw->hsw_hst.hst_opcode);
    hexdump("PKT", hsw->hsw_data, hsw->hsw_used);
    break;
  }
}


static void
http_process_request(http_request_t *hr)
{
  mutex_unlock(&http_server_mutex);
  int return_code = find_route(hr, hr->hr_url);
  mutex_lock(&http_server_mutex);

  http_connection_t *hc = hr->hr_hst.hst_hc;

  if(return_code) {
    // Send a simple response

    while(hc->hc_txbuf_head) {
      cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
    }
    send_response_simple(hc, return_code, !hr->hr_should_keep_alive);
  }

  while(hr->hr_piggyback_503) {

    while(hc->hc_txbuf_head) {
      cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
    }

    if(hc->hc_sock == NULL)
      break;

    send_response_simple(hc, 503, 1);
    hr->hr_piggyback_503--;
  }
}


__attribute__((noreturn))
static void *
http_thread(void *arg)
{
  mutex_lock(&http_server_mutex);
  while(1) {
    http_server_task_t *hst = STAILQ_FIRST(&http_task_queue);
    if(hst == NULL) {
      cond_wait(&http_task_queue_cond, &http_server_mutex);
      continue;
    }

    switch(hst->hst_type) {
    case HST_HTTP_REQ:
      http_process_request((http_request_t *)hst);
      break;
    case HST_WEBSOCKET_PACKET:
      http_process_websocket_packet((http_server_wsp_t *)hst);
      break;
    }

    http_connection_t *hc = hst->hst_hc;
    TAILQ_REMOVE(&hc->hc_tasks, hst, hst_connection_link);
    STAILQ_REMOVE_HEAD(&http_task_queue, hst_global_link);
    free(hst);
    http_connection_maybe_free(hc);
  }
}


static void __attribute__((constructor(300)))
http_init(void)
{
  STAILQ_INIT(&http_task_queue);
  thread_create(http_thread, NULL, 1024, "http", TASK_FPU | TASK_DETACHED, 8);
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
http_response_begin(struct http_request *hr, int status_code,
                    const char *content_type)
{
  http_connection_t *hc = hr->hr_hst.hst_hc;

  mutex_lock(&http_server_mutex);

  while(hc->hc_txbuf_head) {
    cond_wait(&hc->hc_txbuf_cond, &http_server_mutex);
  }

  socket_t *sk = hc->hc_sock;
  if(sk != NULL) {

    hc->hc_txbuf_head =
      make_response(sk, "HTTP/1.1 %d %s\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "Content-Type: %s\r\n"
                    "%s"
                    "\r\n",
                    status_code, http_status_str(status_code),
                    content_type,
                    !hr->hr_should_keep_alive ?
                    "Connection: close\r\n": "");

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
  http_connection_t *hc = hr->hr_hst.hst_hc;

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
