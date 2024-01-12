#include "http.h"

#include <assert.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/param.h>

#include "net/ipv4/tcp.h"
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
#include <mios/atomic.h>

STAILQ_HEAD(http_connection_squeue, http_connection);
STAILQ_HEAD(http_server_task_squeue, http_server_task);
TAILQ_HEAD(http_server_task_queue, http_server_task);

static struct http_server_task_squeue http_task_queue;

static mutex_t http_mutex = MUTEX_INITIALIZER("http");
static cond_t http_task_queue_cond = COND_INITIALIZER("rq");

static void http_stream_write(struct stream *s, const void *buf, size_t size, int flags);

static struct http_connection_squeue closing_websocket_connections;

struct http_connection {

  stream_t s;

  union {
    struct http_parser hc_hp;
    struct {
      struct websocket_parser hc_wp;
      STAILQ_ENTRY(http_connection) hc_ws_close_link;
    };
  };

  int (*hc_ws_cb)(void *opaque,
                  int opcode,
                  void *data,
                  size_t size,
                  http_connection_t *hc,
                  balloc_t *ba);
  void *hc_ws_opaque;

  atomic_t hc_refcount;

  socket_t *hc_sock;

  pbuf_t *hc_txbuf_head;
  pbuf_t *hc_txbuf_tail;
  cond_t hc_txbuf_cond;

  http_server_task_t *hc_task;
  http_server_task_t *hc_ctrl_task;
  struct http_server_task_queue hc_tasks;

  uint8_t hc_output_encoding;
  uint8_t hc_websocket_mode;
  uint8_t hc_ping_counter;

  uint8_t hc_hold;
  uint8_t hc_ws_opcode;
  uint8_t hc_output_mask_bit; // Set to 0x80 if we should do masking

  timer_t hc_timer;

  const http_parser_settings *hc_parser_settings;


};

#define OUTPUT_ENCODING_NONE      0
#define OUTPUT_ENCODING_CHUNKED   1
#define OUTPUT_ENCODING_WEBSOCKET 2

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
http_server_message_begin(http_parser *p)
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
http_server_url(http_parser *p, const char *at, size_t length)
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
http_server_header_field(http_parser *p, const char *at, size_t length)
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
http_server_header_value(http_parser *p, const char *at, size_t length)
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
http_server_headers_complete(http_parser *p)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_task;
  if(hr == NULL)
    return 0;

  if(hr->hr_connection && !strcasecmp(hr->hr_connection, "upgrade") &&
     hr->hr_upgrade && !strcasecmp(hr->hr_upgrade, "websocket")) {
    hc->hc_ws_cb = NULL;
    hc->hc_websocket_mode = 1;
    hr->hr_upgrade_to_websocket = 1;
    http_timer_arm(hc, 5);
    return 2;
  }

  http_timer_arm(hc, 20);
  return 0;
}


static int
http_server_body(http_parser *p, const char *at, size_t length)
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
make_output(socket_t *sk, const char *fmt, ...)
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
    make_output(sk, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n%s\r\n",
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
  atomic_inc(&hc->hc_refcount);
  cond_signal(&http_task_queue_cond);
}

static int
http_server_message_complete(http_parser *p)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_task;

  mutex_lock(&http_mutex);

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

  mutex_unlock(&http_mutex);
  http_timer_arm(hc, 5);
  return 0;
}

static const http_parser_settings server_parser = {
  .on_message_begin    = http_server_message_begin,
  .on_url              = http_server_url,
  .on_header_field     = http_server_header_field,
  .on_header_value     = http_server_header_value,
  .on_headers_complete = http_server_headers_complete,
  .on_body             = http_server_body,
  .on_message_complete = http_server_message_complete,
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
  hsw->hsw_bumpalloc.capacity = capacity;
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
  if(hsw->hsw_bumpalloc.used + to_copy > hsw->hsw_bumpalloc.capacity) {
    return -1;
  }

  uint8_t *dst = hsw->hsw_bumpalloc.data + hsw->hsw_bumpalloc.used;
  memcpy(dst, pkt, to_copy);
  if(wp->wp_header[1] & 0x80) {
    const uint8_t *mask = wp->wp_header + mask_off;
    for(size_t i = 0; i < to_copy; i++) {
      dst[i] ^= mask[i&3];
    }
  }

  consumed += to_copy;
  wp->wp_fragment_used += to_copy;
  hsw->hsw_bumpalloc.used += to_copy;

  if(wp->wp_fragment_used == frag_len) {

    if(wp->wp_header[0] & 0x80) {
      mutex_lock(&http_mutex);
      http_task_enqueue(&hsw->hsw_hst, hc, HST_WEBSOCKET_PACKET);
      mutex_unlock(&http_mutex);

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
    cond_signal(&hc->hc_txbuf_cond);
  }
}


static size_t
http_push_error(http_connection_t *hc, struct pbuf *pb0)
{
  mutex_lock(&http_mutex);
  http_close_locked(hc);
  mutex_unlock(&http_mutex);
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

        r = http_parser_execute(&hc->hc_hp, hc->hc_parser_settings,
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



static struct pbuf *
http_pull(void *opaque)
{
  http_connection_t *hc = opaque;
  mutex_lock(&http_mutex);
  if(hc->hc_hold) {
    mutex_unlock(&http_mutex);
    return NULL;
  }

  pbuf_t *pb = hc->hc_txbuf_head;
  hc->hc_txbuf_head = NULL;
  hc->hc_txbuf_tail = NULL;
  cond_signal(&hc->hc_txbuf_cond);
  mutex_unlock(&http_mutex);

  return pb;
}


void
http_connection_release(http_connection_t *hc)
{
  if(atomic_dec(&hc->hc_refcount))
    return;

  if(hc->hc_txbuf_head)
    pbuf_free(hc->hc_txbuf_head);

  free(hc);
}

static void
http_websocket_enqueue_close(http_connection_t *hc)
{
  atomic_inc(&hc->hc_refcount);
  STAILQ_INSERT_TAIL(&closing_websocket_connections, hc, hc_ws_close_link);
  cond_signal(&http_task_queue_cond);
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

  mutex_lock(&http_mutex);
  http_close_locked(hc);

  if(hc->hc_ws_cb) {
    http_websocket_enqueue_close(hc);
  }
  mutex_unlock(&http_mutex);
  http_connection_release(hc);
}


static const socket_app_fn_t http_sock_fn = {
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

      // If we can't ping for the following reasons, we just don't
      // increase the ping_counter and just retry a ping in 5
      // seconds again.

      // If there's already a frame queued for output, don't ping
      if(hc->hc_txbuf_head != NULL)
        return;

      // If we can't allocated a frame, don't ping
      pbuf_t *pb = pbuf_make(sk->preferred_offset, 0);
      if(pb == NULL)
        return;
      size_t payload_size = 2 + (hc->hc_output_mask_bit >> 5);
      uint8_t *pkt = pbuf_append(pb, 2 + payload_size);
      pkt[0] = 0x80 | WS_OPCODE_PING;
      pkt[1] = 2 | hc->hc_output_mask_bit;
      for(int i = 0 ; i < payload_size; i++) {
        pkt[i + 2] = rand();
      }
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
  mutex_lock(&http_mutex);
  http_timer_locked(hc);
  mutex_unlock(&http_mutex);
}

static http_connection_t *
http_connection_create(enum http_parser_type type,
                       const http_parser_settings *parser_settings)
{
  http_connection_t *hc = xalloc(sizeof(http_connection_t), 0, MEM_MAY_FAIL);
  if(hc == NULL)
    return NULL;

  memset(hc, 0, sizeof(http_connection_t));
  hc->hc_parser_settings = parser_settings;
  atomic_set(&hc->hc_refcount, 1);
  TAILQ_INIT(&hc->hc_tasks);

  http_parser_init(&hc->hc_hp, type);

  hc->hc_hp.data = hc;
  hc->s.write = http_stream_write;
  cond_init(&hc->hc_txbuf_cond, "httpout");

  hc->hc_timer.t_cb = http_timer_cb;
  hc->hc_timer.t_opaque = hc;
  hc->hc_timer.t_name = "http";

  return hc;
}


static error_t
http_accept(socket_t *s)
{
  http_connection_t *hc = http_connection_create(HTTP_REQUEST,
                                                 &server_parser);
  if(hc == NULL)
    return ERR_NO_MEMORY;

  hc->hc_sock = s;
  s->app = &http_sock_fn;
  s->app_opaque = hc;

  http_timer_arm(hc, 5);
  return 0;
}


SERVICE_DEF("http", 80, 0, SERVICE_TYPE_STREAM, http_accept);



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
    while(hc->hc_txbuf_head && hc->hc_sock) {
      cond_wait(&hc->hc_txbuf_cond, &http_mutex);
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
  if(hsw->hsw_hst.hst_opcode == WS_OPCODE_PING) {
    http_websocket_send_locked(hc, WS_OPCODE_PONG,
                               hsw->hsw_bumpalloc.data,
                               hsw->hsw_bumpalloc.used);
    return;
  }

  mutex_unlock(&http_mutex);
  int err = hc->hc_ws_cb(hc->hc_ws_opaque, hsw->hsw_hst.hst_opcode,
                         hsw->hsw_bumpalloc.data, hsw->hsw_bumpalloc.used, hc,
                         &hsw->hsw_bumpalloc);
  mutex_lock(&http_mutex);

  if(err) {
    uint8_t close_reason[2] = {err >> 8, err};
    http_websocket_send_locked(hc, WS_OPCODE_CLOSE, close_reason,
                               sizeof(close_reason));
  }
}

static void
http_process_request(http_request_t *hr)
{
  mutex_unlock(&http_mutex);
  int return_code = find_route(hr, hr->hr_url);
  mutex_lock(&http_mutex);

  http_connection_t *hc = hr->hr_hst.hst_hc;

  if(return_code) {
    // Send a simple response

    while(hc->hc_txbuf_head && hc->hc_sock) {
      cond_wait(&hc->hc_txbuf_cond, &http_mutex);
    }
    if(hc->hc_sock == NULL)
      return;

    send_response_simple(hc, return_code, !hr->hr_should_keep_alive);
  }

  while(hr->hr_piggyback_503) {

    while(hc->hc_txbuf_head && hc->hc_sock) {
      cond_wait(&hc->hc_txbuf_cond, &http_mutex);
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
  mutex_lock(&http_mutex);
  while(1) {
    http_server_task_t *hst = STAILQ_FIRST(&http_task_queue);
    if(hst == NULL) {
      http_connection_t *hc = STAILQ_FIRST(&closing_websocket_connections);
      if(hc != NULL) {
        STAILQ_REMOVE_HEAD(&closing_websocket_connections, hc_ws_close_link);
        mutex_unlock(&http_mutex);
        if(hc->hc_ws_cb) {
          hc->hc_ws_cb(hc->hc_ws_opaque, -1, NULL, 0, hc, NULL);
        }
        mutex_lock(&http_mutex);
        http_connection_release(hc);
        continue;
      }
      cond_wait(&http_task_queue_cond, &http_mutex);
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
    http_connection_release(hc);
  }
}


static void __attribute__((constructor(300)))
http_init(void)
{
  STAILQ_INIT(&http_task_queue);
  STAILQ_INIT(&closing_websocket_connections);
  thread_create(http_thread, NULL, 1024, "http", TASK_FPU | TASK_DETACHED, 8);
}


static const char hexdigit[16] = "0123456789abcdef";

static void
add_chunked_encoding(http_connection_t *hc)
{
  pbuf_t *pb = hc->hc_txbuf_head;

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
  hc->hc_txbuf_head = pb;

  pbuf_t *tail = hc->hc_txbuf_tail;
  memcpy(tail->pb_data + tail->pb_offset + tail->pb_buflen, "\r\n", 2);
  tail->pb_buflen += 2;
  pb->pb_pktlen += 2;
}


static void
add_websocket_framing(http_connection_t *hc, int fin)
{
  pbuf_t *pb = hc->hc_txbuf_head;
  size_t len = pb->pb_pktlen;

  if(hc->hc_output_mask_bit) {

    assert(pb->pb_offset >= 4);
    uint8_t *m = pbuf_data(pb, 0) - 4;
    uint32_t r = rand();
    memcpy(m, &r, sizeof(uint32_t));

    int o = 0;
    for(pbuf_t *p = pb; p != NULL; p = p->pb_next) {
      uint8_t *d = pbuf_data(p, 0);
      for(size_t i = 0; i < p->pb_buflen; i++) {
        d[i] ^= m[o & 3];
        o++;
      }
    }

    pb->pb_offset -= 4;
    pb->pb_buflen += 4;
    pb->pb_pktlen += 4;
  }

  assert(pb->pb_offset >= 4);
  pb->pb_offset -= 4;

  uint8_t *hdr;
  if(len < 126) {
    memmove(pbuf_data(pb, 2), pbuf_data(pb, 4), pb->pb_buflen);
    pb->pb_buflen += 2;
    pb->pb_pktlen += 2;
    hdr = pbuf_data(pb, 0);
    hdr[1] = len | hc->hc_output_mask_bit;
  } else {
    pb->pb_buflen += 4;
    pb->pb_pktlen += 4;

    hdr = pbuf_data(pb, 0);
    hdr[1] = 126 | hc->hc_output_mask_bit;
    hdr[2] = len >> 8;
    hdr[3] = len;
  }
  hdr[0] = hc->hc_ws_opcode | (fin ? 0x80 : 0);
  hc->hc_ws_opcode = 0;
}



static void
http_stream_release_packet(http_connection_t *hc, int fin)
{
  socket_t *sk = hc->hc_sock;
  if(sk == NULL)
    return;

  switch(hc->hc_output_encoding) {
  case OUTPUT_ENCODING_CHUNKED:
    add_chunked_encoding(hc);
    break;
  case OUTPUT_ENCODING_WEBSOCKET:
    add_websocket_framing(hc, fin);
    break;
  }
  hc->hc_hold = 0;

  sk->net->event(sk->net_opaque, SOCKET_EVENT_WAKEUP);
}


static void
http_stream_write(struct stream *s, const void *buf, size_t size, int flags)
{
  http_connection_t *hc = (http_connection_t *)s;

  mutex_lock(&http_mutex);

  socket_t *sk = hc->hc_sock;

  if(buf == NULL) {
    // Flush

    if(sk != NULL && hc->hc_txbuf_head != NULL) {
      http_stream_release_packet(hc, 1);
    }

  } else {

    while(size) {

      sk = hc->hc_sock;

      if(sk == NULL)
        break;

      if(hc->hc_txbuf_head == NULL) {
        hc->hc_txbuf_head = pbuf_make(sk->preferred_offset + 8, 1);
        hc->hc_txbuf_tail = hc->hc_txbuf_head;
        hc->hc_hold = 1;
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
        http_stream_release_packet(hc, 0);
        cond_wait(&hc->hc_txbuf_cond, &http_mutex);
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
  mutex_unlock(&http_mutex);
}

struct stream *
http_response_begin(struct http_request *hr, int status_code,
                    const char *content_type)
{
  http_connection_t *hc = hr->hr_hst.hst_hc;

  mutex_lock(&http_mutex);

  while(hc->hc_txbuf_head && hc->hc_sock) {
    cond_wait(&hc->hc_txbuf_cond, &http_mutex);
  }

  socket_t *sk = hc->hc_sock;
  if(sk != NULL) {

    hc->hc_txbuf_head =
      make_output(sk, "HTTP/1.1 %d %s\r\n"
                  "Transfer-Encoding: chunked\r\n"
                  "Content-Type: %s\r\n"
                  "%s"
                  "\r\n",
                  status_code, http_status_str(status_code),
                  content_type,
                  !hr->hr_should_keep_alive ?
                  "Connection: close\r\n": "");

    hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_WAKEUP);

    while(hc->hc_txbuf_head && hc->hc_sock) {
      cond_wait(&hc->hc_txbuf_cond, &http_mutex);
    }
    hc->hc_output_encoding = OUTPUT_ENCODING_CHUNKED;
  }

  mutex_unlock(&http_mutex);
  return &hc->s;
}

int
http_response_end(struct http_request *hr)
{
  http_connection_t *hc = hr->hr_hst.hst_hc;
  pbuf_t *pb;

  mutex_lock(&http_mutex);

  if(hc->hc_hold)
    http_stream_release_packet(hc, 1);

  while(hc->hc_txbuf_head && hc->hc_sock) {
    cond_wait(&hc->hc_txbuf_cond, &http_mutex);
  }

  socket_t *sk = hc->hc_sock;
  if(sk != NULL && hc->hc_output_encoding == OUTPUT_ENCODING_CHUNKED) {
    pb = pbuf_make(sk->preferred_offset, 1);
    memcpy(pbuf_data(pb, 0), "0\r\n\r\n", 5);

    pb->pb_pktlen += 5;
    pb->pb_buflen += 5;

    hc->hc_txbuf_head = pb;
    hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_WAKEUP);
  }

  hc->hc_output_encoding = 0;
  mutex_unlock(&http_mutex);
  return 0;
}

struct stream *
http_websocket_output_begin(http_connection_t *hc, int opcode)
{
  mutex_lock(&http_mutex);

  while(hc->hc_txbuf_head && hc->hc_sock) {
    cond_wait(&hc->hc_txbuf_cond, &http_mutex);
  }
  hc->hc_output_encoding = OUTPUT_ENCODING_WEBSOCKET;
  hc->hc_ws_opcode = opcode;
  mutex_unlock(&http_mutex);
  return &hc->s;
}

int
http_websocket_output_end(http_connection_t *hc)
{
  mutex_lock(&http_mutex);

  if(hc->hc_hold)
    http_stream_release_packet(hc, 1);

  while(hc->hc_txbuf_head && hc->hc_sock) {
    cond_wait(&hc->hc_txbuf_cond, &http_mutex);
  }
  hc->hc_output_encoding = 0;
  mutex_unlock(&http_mutex);
  return 0;

}


#define WSGUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

int
http_request_accept_websocket(http_request_t *hr,
                              int (*cb)(void *opaque,
                                        int opcode,
                                        void *data,
                                        size_t size,
                                        http_connection_t *hc,
                                        balloc_t *ba),
                              void *opaque,
                              http_connection_t **hcp)
{
  SHA1_CTX shactx;

  if(hr->hr_wskey == NULL || !hr->hr_upgrade_to_websocket)
    return HTTP_STATUS_BAD_REQUEST;

  SHA1Init(&shactx);
  SHA1Update(&shactx, (const void *)hr->hr_wskey, strlen(hr->hr_wskey));
  SHA1Update(&shactx, (const void *)WSGUID, strlen(WSGUID));

  uint8_t *digest = balloc_alloc(&hr->hr_bumpalloc, 20);
  char *sig = balloc_alloc(&hr->hr_bumpalloc, 64);
  if(digest == NULL || sig == NULL) {
    return HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
  }

  SHA1Final(digest, &shactx);
  base64_encode(sig, 64, digest, 20);

  http_connection_t *hc = hr->hr_hst.hst_hc;

  mutex_lock(&http_mutex);

  while(hc->hc_txbuf_head && hc->hc_sock) {
    cond_wait(&hc->hc_txbuf_cond, &http_mutex);
  }

  socket_t *sk = hc->hc_sock;
  if(sk != NULL) {
    hc->hc_txbuf_head = make_output(sk, "HTTP/1.1 %d %s\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Sec-WebSocket-Accept: %s\r\n"
                                    "\r\n",
                                    101, http_status_str(101),
                                    sig);
    hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_WAKEUP);
  }

  hc->hc_ws_cb = cb;
  hc->hc_ws_opaque = opaque;

  mutex_unlock(&http_mutex);
  if(hcp) {
    atomic_inc(&hc->hc_refcount);
    *hcp = hc;
  }
  return 0;
}



static int
http_client_headers_complete(http_parser *p)
{
  http_connection_t *hc = p->data;

  if(p->status_code == HTTP_STATUS_SWITCHING_PROTOCOLS) {
    hc->hc_websocket_mode = 1;
    http_timer_arm(hc, 5);
  } else {
    mutex_lock(&http_mutex);
    http_websocket_enqueue_close(hc);
    http_close_locked(hc);
    mutex_unlock(&http_mutex);
    return 1;
  }
  return 0;
}


static const http_parser_settings client_parser = {
  .on_headers_complete = http_client_headers_complete,
};


http_connection_t *
http_websocket_create(int (*cb)(void *opaque,
                                int opcode,
                                void *data,
                                size_t size,
                                http_connection_t *hc,
                                balloc_t *ba),
                      void *opaque)
{
  http_connection_t *hc = http_connection_create(HTTP_RESPONSE,
                                                 &client_parser);
  if(hc == NULL)
    return NULL;

  socket_t *sk = tcp_create_socket("websocket");
  if(sk == NULL) {
    free(hc);
    return NULL;
  }

  atomic_inc(&hc->hc_refcount); // refcount for returned pointer

  hc->hc_output_mask_bit = 0x80;
  hc->hc_sock = sk;
  hc->hc_ws_cb = cb;
  hc->hc_ws_opaque = opaque;

  sk->app = &http_sock_fn;
  sk->app_opaque = hc;

  return hc;
}

void
http_websocket_start(http_connection_t *hc, uint32_t addr,
                     uint16_t port, const char *path)
{
  tcp_connect(hc->hc_sock, addr, port);

  mutex_lock(&http_mutex);

  hc->hc_txbuf_head =
    make_output(hc->hc_sock,
                "GET %s HTTP/1.1\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: Aoetmg2mBDsoQUGZN05WLQ==\r\n"
                "\r\n",
                path);

  hc->hc_sock->net->event(hc->hc_sock->net_opaque, SOCKET_EVENT_WAKEUP);

  mutex_unlock(&http_mutex);
}


void
http_websocket_close(http_connection_t *hc, uint16_t status_code,
                     const char *message)
{
  mutex_lock(&http_mutex);

  uint8_t close_reason[2] = {status_code >> 8, status_code};
  http_websocket_send_locked(hc, WS_OPCODE_CLOSE, close_reason,
                             sizeof(close_reason));
  mutex_unlock(&http_mutex);
}
