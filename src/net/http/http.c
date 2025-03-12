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
#include "http_util.h"
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


typedef struct http_payload {

  balloc_t hp_bumpalloc;

} http_payload_t;

#define HTTP_OUTPUT_BUFFER_SIZE 256

struct http_connection {

  stream_t hc_response_stream;

  union {
    struct http_parser hc_hp;
    struct websocket_parser hc_wp;
  };

  int (*hc_ws_cb)(void *opaque,
                  int opcode,
                  void *data,
                  size_t size,
                  http_connection_t *hc,
                  balloc_t *ba);
  void *hc_ws_opaque;

  atomic_t hc_refcount;

  stream_t *hc_socket;

  void *hc_payload;
  void *hc_ctrl;

  uint8_t hc_output_encoding;
  uint8_t hc_websocket_mode;
  uint8_t hc_ping_counter;

  uint8_t hc_hold : 1;
  uint8_t hc_notify : 1;
  uint8_t hc_ws_rx_opcode;
  uint8_t hc_ws_tx_opcode;
  uint8_t hc_output_mask_bit; // Set to 0x80 if we should do masking
  uint8_t hc_index;

  timer_t hc_timer;

  uint8_t hc_output_buffer[HTTP_OUTPUT_BUFFER_SIZE];
  size_t hc_output_buffer_used;

  const http_parser_settings *hc_parser_settings;
  const char *hc_close_reason;
};


#define MAX_HTTP_SERVER_CONNECTIONS 16

typedef struct {

  pollset_t hs_pollset[MAX_HTTP_SERVER_CONNECTIONS + 1];
  http_connection_t *hs_connections[MAX_HTTP_SERVER_CONNECTIONS];
  struct timer_list hs_timers;
  mutex_t hs_mutex;
  cond_t hs_cond;
  uint8_t hs_update_pollset;

} http_server_t;

static http_server_t g_http_server;


#define OUTPUT_ENCODING_NONE      0
#define OUTPUT_ENCODING_CHUNKED   1
#define OUTPUT_ENCODING_WEBSOCKET 2

#define HEADER_HOST                   0x1
#define HEADER_SEC_WEBSOCKET_KEY      0x2
#define HEADER_CONTENT_TYPE           0x4
#define HEADER_CONNECTION             0x8
#define HEADER_UPGRADE                0x10
#define HEADER_SEC_WEBSOCKET_PROTOCOL 0x20

#define HEADER_ALL (HEADER_HOST | \
                    HEADER_SEC_WEBSOCKET_KEY | \
                    HEADER_CONTENT_TYPE | \
                    HEADER_CONNECTION | \
                    HEADER_UPGRADE | \
                    HEADER_SEC_WEBSOCKET_PROTOCOL)



void
http_connection_retain(http_connection_t *hc)
{
  atomic_inc(&hc->hc_refcount);
}

void
http_connection_release(http_connection_t *hc)
{
  if(atomic_dec(&hc->hc_refcount))
    return;

  free(hc);
}


static ssize_t
http_response_chunk_send(http_connection_t *hc, const void *data, size_t len)
{
  stprintf(hc->hc_socket, "%x\r\n", len);
  stream_write(hc->hc_socket, data, len, 0);
  stprintf(hc->hc_socket, "\r\n");
  return len;
}


static ssize_t
http_response_write(stream_t *s, const void *buf, size_t size, int flags)
{
  http_connection_t *hc = (http_connection_t *)s;
  size_t total = size;

  while(size) {

    if(hc->hc_output_buffer_used == 0 && size > HTTP_OUTPUT_BUFFER_SIZE) {
      http_response_chunk_send(hc, buf, size);
      return total;
    }

    size_t to_copy = MIN(HTTP_OUTPUT_BUFFER_SIZE - hc->hc_output_buffer_used,
                         size);
    memcpy(hc->hc_output_buffer + hc->hc_output_buffer_used, buf, to_copy);

    buf += to_copy;
    size -= to_copy;
    hc->hc_output_buffer_used += to_copy;

    if(hc->hc_output_buffer_used == HTTP_OUTPUT_BUFFER_SIZE) {
      http_response_chunk_send(hc, hc->hc_output_buffer,
                               HTTP_OUTPUT_BUFFER_SIZE);
      hc->hc_output_buffer_used = 0;
    }
  }
  return total;
}


static void
http_response_close(stream_t *s)
{
  http_connection_t *hc = (http_connection_t *)s;

  if(hc->hc_output_buffer_used) {
    http_response_chunk_send(hc, hc->hc_output_buffer,
                             hc->hc_output_buffer_used);
    hc->hc_output_buffer_used = 0;
  }
  stprintf(hc->hc_socket, "0\r\n\r\n");
  stream_flush(hc->hc_socket);
}


static const stream_vtable_t http_response_vtable = {
  .write = http_response_write,
  .close = http_response_close,
};


struct stream *
http_response_begin(struct http_request *hr, int status_code,
                    const char *content_type)
{
  http_connection_t *hc = hr->hr_hc;

  stprintf(hc->hc_socket,
           "HTTP/1.1 %d %s\r\n"
           "Transfer-Encoding: chunked\r\n"
           "Content-Type: %s\r\n"
           "%s"
           "\r\n",
           status_code, http_status_str(status_code),
           content_type,
           !hr->hr_should_keep_alive ?
           "Connection: close\r\n": "");

  hc->hc_response_stream.vtable = &http_response_vtable;
  return &hc->hc_response_stream;
}


static void *
payload_acquire(void **p, size_t capacity)
{
  if(*p)
    return *p;

  balloc_t *ba = xalloc(sizeof(balloc_t) + capacity, 0, MEM_MAY_FAIL);
  if(ba == NULL)
    return NULL;

  *p = ba;
  memset(ba, 0, sizeof(balloc_t));
  ba->capacity = capacity;
  return ba;
}


static ssize_t
websocket_send_fragment(http_connection_t *hc, void *buf, size_t size, int fin,
                        int opcode, int flags)
{
  uint8_t hdr[8];
  int hlen = 2;
  if(size < 126) {
    hdr[1] = size | hc->hc_output_mask_bit;
  } else {
    hdr[1] = 126 | hc->hc_output_mask_bit;
    hdr[2] = size >> 8;
    hdr[3] = size;
    hlen = 4;
  }

  if(hc->hc_output_mask_bit) {
    memset(hdr + hlen, 0, 4);
    hlen += 4;
  }

  hdr[0] = opcode | (fin ? 0x80 : 0);
  //  hc->hc_ws_tx_opcode = 0;

  struct iovec iov[2];
  iov[0].iov_base = hdr;
  iov[0].iov_len = hlen;
  iov[1].iov_base = buf;
  iov[1].iov_len = size;
  return stream_writev(hc->hc_socket, iov, 2, flags);
}


static ssize_t
websocket_parser_execute(http_connection_t *hc,
                         const uint8_t *pkt, size_t total_len)
{
  websocket_parser_t *wp = &hc->hc_wp;

  size_t pkt_len = total_len;
  size_t consumed = 0;

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
    return ERR_INVALID_LENGTH;

  const int opcode = wp->wp_header[0] & 0xf;

  balloc_t *ba;

  if(opcode & 0x8) {

    if(!(wp->wp_header[0] & 0x80) || frag_len > 125)
      return ERR_MALFORMED;

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
    ba = payload_acquire(&hc->hc_ctrl, frag_len);
  } else {
    if(opcode)
      hc->hc_ws_rx_opcode = opcode;
    ba = payload_acquire(&hc->hc_payload, 4096);
  }

  if(ba == NULL) {
    return ERR_NO_BUFFER;
  }

  const size_t to_copy = MIN(frag_len - wp->wp_fragment_used, pkt_len);
  if(ba->used + to_copy > ba->capacity) {
    return ERR_NO_BUFFER;
  }

  uint8_t *dst = ba->data + ba->used;
  memcpy(dst, pkt, to_copy);
  if(wp->wp_header[1] & 0x80) {
    const uint8_t *mask = wp->wp_header + mask_off;
    for(size_t i = 0; i < to_copy; i++) {
      dst[i] ^= mask[(i + ba->used)&3];
    }
  }

  consumed += to_copy;
  wp->wp_fragment_used += to_copy;
  ba->used += to_copy;

  if(wp->wp_fragment_used == frag_len) {

    wp->wp_header_len = 0;
    wp->wp_fragment_used = 0;
    if(wp->wp_header[0] & 0x80) { // FIN bit
      int result = 0;

      if(opcode & 0x8) {
        // Handle control messages in a special path
        switch(opcode) {

        case WS_OPCODE_PING:   // Echo back payload
          websocket_send_fragment(hc, ba->data, ba->used, 1,
                                  WS_OPCODE_PONG, 0);
          break;
        case WS_OPCODE_CLOSE:
          if(hc->hc_ws_cb != NULL) {
            // Propagate close message to user callback
            result = hc->hc_ws_cb(hc->hc_ws_opaque, WS_OPCODE_CLOSE,
                                  ba->data, ba->used, hc, ba);
          }

          // Echo back close message (send back received status-code if any)
          if(result == 0 && ba->used > 2) {
            const uint8_t *u8 = ba->data;
            result = (u8[0] << 8) | u8[1];
          }

          if(result == 0) // Nothing, send normal close
            result = WS_STATUS_NORMAL_CLOSE;
          break;

        default:
          // Control opcode we don't understand, bail
          result = WS_STATUS_PROTOCOL_ERROR;
        }

      } else if(hc->hc_ws_cb != NULL) {
        result = hc->hc_ws_cb(hc->hc_ws_opaque, hc->hc_ws_rx_opcode,
                              ba->data, ba->used, hc, ba);
      }

      if(result) {
        uint8_t payload[2] = {result >> 8, result};
        websocket_send_fragment(hc, payload, sizeof(payload), 1,
                                WS_OPCODE_CLOSE, 0);
        return ERR_NOT_CONNECTED;
      }

      if(opcode & 0x8) {
        free(hc->hc_ctrl);
        hc->hc_ctrl = NULL;
      } else {
        free(hc->hc_payload);
        hc->hc_payload = NULL;
      }
    }
  }

  return opcode == WS_OPCODE_CLOSE ? ERR_NOT_CONNECTED : consumed;
}


static void
http_timer_arm(http_connection_t *hc, http_server_t *hs, int seconds)
{
  timer_arm_on_queue(&hc->hc_timer, clock_get() + seconds * 1000000,
                     &hs->hs_timers);
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
  hr->hr_hc = hc;
  hc->hc_payload = hr;
  return 0;
}


static int
http_server_url(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_payload;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  if(!balloc_append_data(&hr->hr_bumpalloc, at, length,
                         (void **)&hr->hr_url, NULL)) {
    hr->hr_header_err = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
  }
  return 0;
}

__attribute__((noinline))
static int
header_append(http_request_t *hr, const char *str, size_t len, void **p)
{
  void *x = balloc_append_data(&hr->hr_bumpalloc, str, len, p, NULL);
  if(x == NULL)
    hr->hr_header_err = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
  return 0;
}


static int
header_host(void *opaque, const char *str, size_t len)
{
  http_request_t *hr = opaque;
  return header_append(hr, str, len, (void **)&hr->hr_host);
}

static int
header_sec_websocket_key(void *opaque, const char *str, size_t len)
{
  http_request_t *hr = opaque;
  return header_append(hr, str, len, (void **)&hr->hr_wskey);
}

static int
header_content_type(void *opaque, const char *str, size_t len)
{
  http_request_t *hr = opaque;
  return header_append(hr, str, len, (void **)&hr->hr_content_type);
}

static int
header_connection(void *opaque, const char *str, size_t len)
{
  http_request_t *hr = opaque;
  return header_append(hr, str, len, (void **)&hr->hr_connection);
}

static int
header_upgrade(void *opaque, const char *str, size_t len)
{
  http_request_t *hr = opaque;
  return header_append(hr, str, len, (void **)&hr->hr_upgrade);
}

static int
header_sec_websocket_protocol(void *opaque, const char *str, size_t len)
{
  http_request_t *hr = opaque;
  return header_append(hr, str, len, (void **)&hr->hr_wsproto);
}

static const http_header_callback_t server_headers[] = {
  { "host", header_host },
  { "sec-websocket-key", header_sec_websocket_key },
  { "content-type", header_content_type },
  { "connection", header_connection },
  { "upgrade", header_upgrade },
  { "sec-websocket-protocol", header_sec_websocket_protocol },
};

static int
http_server_header_field(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_payload;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  return http_match_header_field(&hr->hr_header_matcher, at, length,
                                 server_headers, ARRAYSIZE(server_headers));
}

static int
http_server_header_value(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_payload;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  return http_match_header_value(&hr->hr_header_matcher, at, length,
                                 server_headers, ARRAYSIZE(server_headers),
                                 hr);
}


static int
http_server_headers_complete(http_parser *p)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_payload;
  if(hr == NULL)
    return 0;

  if(hr->hr_connection && !strcasecmp(hr->hr_connection, "upgrade") &&
     hr->hr_upgrade && !strcasecmp(hr->hr_upgrade, "websocket")) {
    hc->hc_ws_cb = NULL;
    hc->hc_websocket_mode = 1;
    hr->hr_upgrade_to_websocket = 1;
    http_timer_arm(hc, &g_http_server, 5);
    return 2;
  }

  http_timer_arm(hc, &g_http_server, 20);
  return 0;
}


static int
http_server_body(http_parser *p, const char *at, size_t length)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_payload;
  if(hr == NULL || hr->hr_header_err)
    return 0;

  if(!balloc_append_data(&hr->hr_bumpalloc, at, length,
                         &hr->hr_body, &hr->hr_body_size)) {
    hr->hr_header_err = HTTP_STATUS_PAYLOAD_TOO_LARGE;
  }

  return 0;
}


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
send_simple_output(http_connection_t *hc, int http_status_code,
                   int do_close)
{
  stprintf(hc->hc_socket,
           "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n%s\r\n",
           http_status_code, http_status_str(http_status_code),
           do_close ? "Connection: close\r\n": "");
  stream_flush(hc->hc_socket);

}

static void
http_process_request(http_request_t *hr, http_connection_t *hc)
{
  int http_status_code = find_route(hr, hr->hr_url);

  if(http_status_code) {
    send_simple_output(hc, http_status_code, !hr->hr_should_keep_alive);
    return;
  }
}


static int
http_server_message_complete(http_parser *p)
{
  http_connection_t *hc = p->data;
  http_request_t *hr = (http_request_t *)hc->hc_payload;

  hr->hr_should_keep_alive = http_should_keep_alive(p);

  http_process_request(hr, hc);
  hc->hc_payload = NULL;
  free(hr);
  http_timer_arm(hc, &g_http_server, 5);
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


static void
http_connection_shutdown(http_connection_t *hc, http_server_t *hs,
                         const char *reason)
{
  if(hc->hc_ws_cb) {
    hc->hc_ws_cb(hc->hc_ws_opaque, WS_OPCODE_DISCONNECT,
                 (char *)reason, 0, hc, NULL);
    hc->hc_ws_cb = NULL;
  }

  stream_close(hc->hc_socket);
  hs->hs_pollset[hc->hc_index].type = POLL_NONE;

  timer_disarm(&hc->hc_timer);

  hs->hs_connections[hc->hc_index] = NULL;
  http_connection_release(hc);

  free(hc->hc_ctrl);
  hc->hc_ctrl = NULL;

  free(hc->hc_payload);
  hc->hc_payload = NULL;
}

static void
http_timer_cb(void *opaque, uint64_t now)
{
  http_connection_t *hc = opaque;

  if(hc->hc_websocket_mode && hc->hc_ping_counter < 3) {

    http_timer_arm(hc, &g_http_server, 5);
    uint8_t payload[2] = {0x13, 0x37};
    ssize_t r =
      websocket_send_fragment(hc, payload, sizeof(payload), 1, WS_OPCODE_PING,
                              STREAM_WRITE_NO_WAIT | STREAM_WRITE_ALL);
    if(r > 0)
      hc->hc_ping_counter++;

    return;
  }
  http_connection_shutdown(hc, &g_http_server, "Timeout");
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

  http_parser_init(&hc->hc_hp, type);

  hc->hc_hp.data = hc;

  hc->hc_timer.t_cb = http_timer_cb;
  hc->hc_timer.t_opaque = hc;
  hc->hc_timer.t_name = "http";

  return hc;
}


static error_t
http_connection_serve(http_connection_t *hc, http_server_t *hs)
{
  while(1) {

    void *buf;
    ssize_t bytes = stream_peek(hc->hc_socket, &buf, 0);
    if(bytes < 1)
      return bytes;

    ssize_t consumed;

    if(hc->hc_websocket_mode) {
      websocket_parser_t *wp = &hc->hc_wp;

      if(hc->hc_websocket_mode == 1) {
        memset(wp, 0, sizeof(websocket_parser_t));
        hc->hc_websocket_mode++;
      }

      consumed = websocket_parser_execute(hc, buf, bytes);

      } else {

      consumed = http_parser_execute(&hc->hc_hp, hc->hc_parser_settings,
                                     buf, bytes);
      if(hc->hc_hp.http_errno) {
        return ERR_NOT_CONNECTED;
      }
    }

    if(consumed < 1)
      return consumed;

    stream_drop(hc->hc_socket, consumed);
  }
}


static void
http_update_pollset(http_server_t *hs)
{
  for(size_t i = 0; i < MAX_HTTP_SERVER_CONNECTIONS; i++) {
    http_connection_t *hc = hs->hs_connections[i];
    if(hc != NULL) {
      hs->hs_pollset[i].obj = hc->hc_socket;

      if(hs->hs_pollset[i].type == POLL_NONE) {
        http_timer_arm(hc, hs, 5);
      }

      hs->hs_pollset[i].type = POLL_STREAM_READ;
    } else {
      hs->hs_pollset[i].type = POLL_NONE;
    }
  }
}

__attribute__((noreturn))
static void *
http_thread(void *arg)
{
  http_server_t *hs = arg;

  hs->hs_pollset[MAX_HTTP_SERVER_CONNECTIONS].obj = &hs->hs_cond;
  hs->hs_pollset[MAX_HTTP_SERVER_CONNECTIONS].type = POLL_COND;

  mutex_lock(&hs->hs_mutex);

  while(1) {
    timer_t *t = LIST_FIRST(&hs->hs_timers);

    if(hs->hs_update_pollset) {
      hs->hs_update_pollset = 0;
      http_update_pollset(hs);
    }

    int idx = poll(hs->hs_pollset, MAX_HTTP_SERVER_CONNECTIONS + 1,
                   &hs->hs_mutex, t != NULL ? t->t_expire : INT64_MAX);

    mutex_unlock(&hs->hs_mutex);
    if(idx >= 0 && idx < MAX_HTTP_SERVER_CONNECTIONS) {
      http_connection_t *hc = hs->hs_connections[idx];
      error_t err = http_connection_serve(hc, hs);
      if(err)
        http_connection_shutdown(hc, hs, error_to_string(err));
    }

    timer_dispatch(&hs->hs_timers, clock_get());
    mutex_lock(&hs->hs_mutex);

  }
}

static void __attribute__((constructor(300)))
http_init(void)
{
  http_server_t *hs = &g_http_server;
  thread_create(http_thread, hs, 4096, "http", TASK_DETACHED, 9);
}


static error_t
http_connection_link(http_connection_t *hc)
{
  http_server_t *hs = &g_http_server;

  mutex_lock(&hs->hs_mutex);
  for(int i = 0; i < MAX_HTTP_SERVER_CONNECTIONS; i++) {
    if(hs->hs_connections[i] == NULL) {
      hc->hc_index = i;
      hs->hs_connections[i] = hc;
      hs->hs_update_pollset = 1;
      cond_signal(&hs->hs_cond);
      mutex_unlock(&hs->hs_mutex);
      return 0;
    }
  }
  mutex_unlock(&hs->hs_mutex);
  return ERR_NOSPC;
}








static error_t
http_accept(stream_t *s)
{
  http_connection_t *hc = http_connection_create(HTTP_REQUEST,
                                                 &server_parser);
  if(hc == NULL)
    return ERR_NO_MEMORY;

  hc->hc_socket = s;

  error_t err = http_connection_link(hc);
  if(err)
    free(hc);

  return err;
}

SERVICE_DEF_STREAM("http", 80, http_accept);


static ssize_t
websocket_output_write(stream_t *s, const void *buf, size_t size, int flags)
{
  http_connection_t *hc = (http_connection_t *)s;

  if(buf == NULL)
    return 0;

  while(size) {

    if(hc->hc_output_buffer_used == HTTP_OUTPUT_BUFFER_SIZE) {
      websocket_send_fragment(hc, hc->hc_output_buffer,
                              HTTP_OUTPUT_BUFFER_SIZE, 0,
                              hc->hc_ws_tx_opcode, 0);
      hc->hc_ws_tx_opcode = 0;
      hc->hc_output_buffer_used = 0;
    }

    size_t to_copy = MIN(HTTP_OUTPUT_BUFFER_SIZE - hc->hc_output_buffer_used,
                         size);
    memcpy(hc->hc_output_buffer + hc->hc_output_buffer_used, buf, to_copy);

    buf += to_copy;
    size -= to_copy;
    hc->hc_output_buffer_used += to_copy;
  }

  return size;
}


static void
websocket_output_close(stream_t *s)
{
  http_connection_t *hc = (http_connection_t *)s;

  websocket_send_fragment(hc, hc->hc_output_buffer,
                          hc->hc_output_buffer_used, 1,
                          hc->hc_ws_tx_opcode, STREAM_WRITE_ALL);
  hc->hc_output_buffer_used = 0;
  hc->hc_ws_tx_opcode = 0;
}


static const stream_vtable_t websocket_output_vtable = {
  .write = websocket_output_write,
  .close = websocket_output_close,
};


struct stream *
http_websocket_output_begin(http_connection_t *hc, int opcode)
{
  hc->hc_ws_tx_opcode = opcode;
  hc->hc_response_stream.vtable = &websocket_output_vtable;
  return &hc->hc_response_stream;
}


ssize_t
http_websocket_sendv(http_connection_t *hc, int opcode,
                     struct iovec *iov0, size_t iovcnt,
                     int flags)
{
  struct iovec iov[iovcnt + 1];

  size_t size = 0;
  for(size_t i = 0; i < iovcnt; i++) {
    size += iov0[i].iov_len;
  }

  uint8_t hdr[8];
  int hlen = 2;
  if(size < 126) {
    hdr[1] = size | hc->hc_output_mask_bit;
  } else {
    hdr[1] = 126 | hc->hc_output_mask_bit;
    hdr[2] = size >> 8;
    hdr[3] = size;
    hlen = 4;
  }

  if(hc->hc_output_mask_bit) {
    memset(hdr + hlen, 0, 4);
    hlen += 4;
  }

  hdr[0] = opcode | 0x80;

  iov[0].iov_base = hdr;
  iov[0].iov_len = hlen;

  for(size_t i = 0; i < iovcnt; i++) {
    iov[1 + i].iov_base = iov0[i].iov_base;
    iov[1 + i].iov_len = iov0[i].iov_len;
  }
  return stream_writev(hc->hc_socket, iov, iovcnt + 1, flags);
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
                              http_connection_t **hcp,
                              const char *protocol)
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

  http_connection_t *hc = hr->hr_hc;

  hc->hc_ws_cb = cb;
  hc->hc_ws_opaque = opaque;

  if(hcp) {
    atomic_inc(&hc->hc_refcount);
    *hcp = hc;
  }

  stprintf(hc->hc_socket,
           "HTTP/1.1 %d %s\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Accept: %s\r\n",
           101, http_status_str(101),
           sig);

  if(protocol) {
    stprintf(hc->hc_socket,
             "Sec-WebSocket-Protocol: %s\r\n",
             protocol);
  }
  stream_write(hc->hc_socket, "\r\n", 2, 0);
  return 0;
}



static int
websocket_response_headers_complete(http_parser *p)
{
  http_connection_t *hc = p->data;

  if(p->status_code == HTTP_STATUS_SWITCHING_PROTOCOLS) {
    hc->hc_ws_cb(hc->hc_ws_opaque, WS_OPCODE_OPEN,
                 NULL, 0, hc, NULL);
    hc->hc_websocket_mode = 1;
    http_timer_arm(hc, &g_http_server, 5);
  } else {
    http_connection_shutdown(hc, &g_http_server, "No websocket endpoint");
    return 1;
  }
  return 0;
}


static const http_parser_settings websocket_response_parser = {
  .on_headers_complete = websocket_response_headers_complete,
};

http_connection_t *
http_websocket_create(int (*cb)(void *opaque,
                                int opcode,
                                void *data,
                                size_t size,
                                http_connection_t *hc,
                                balloc_t *ba),
                      void *opaque,
                      const char *name)
{
  http_connection_t *hc = http_connection_create(HTTP_RESPONSE,
                                                 &websocket_response_parser);
  if(hc == NULL)
    return NULL;

  stream_t *sk = tcp_create_socket(name, 2048, 2048);
  if(sk == NULL) {
    free(hc);
    return NULL;
  }

  atomic_inc(&hc->hc_refcount); // refcount for returned pointer

  hc->hc_output_mask_bit = 0x80;
  hc->hc_socket = sk;
  hc->hc_ws_cb = cb;
  hc->hc_ws_opaque = opaque;

  return hc;
}


error_t
http_websocket_start(http_connection_t *hc, uint32_t addr,
                     uint16_t port, const char *path,
                     const char *protocol)
{
  tcp_connect(hc->hc_socket, addr, port);

  stprintf(hc->hc_socket,
           "GET %s HTTP/1.1\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: Aoetmg2mBDsoQUGZN05WLQ==\r\n"
           "%s%s%s"
           "\r\n",
           path,
           protocol ? "Sec-WebSocket-Protocol: " : "",
           protocol ?: "",
           protocol ? "\r\n" : "");

  return http_connection_link(hc);
}


void
http_websocket_close(http_connection_t *hc, uint16_t status_code,
                     const char *message)
{
  uint8_t payload[2] = {status_code >> 8, status_code};

  websocket_send_fragment(hc, payload, sizeof(payload), 1, WS_OPCODE_CLOSE, 0);
  stream_close(hc->hc_socket);
}
