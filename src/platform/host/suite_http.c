/*
 * "http": the HTTP server (net/http/http.c) and its websockets, served on
 * a vnet to a scripted TCP client (sim_tcp.h), in virtual time.
 *
 * Every way a connection ends goes through http_connection_shutdown():
 * the server's idle timer after a response (with "Connection: close" it
 * says so, and then waits for the timer, 5 s), the client closing (FIN,
 * RST, a websocket CLOSE frame), the websocket ping timeout, and a route
 * that keeps its own reference to the connection and drops it later. The
 * host's heap unmaps a block when it is freed (cpu/host/heap_mmap.c), so
 * any of them touching the connection after its last reference is gone
 * faults on the spot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>

#include <mios/stream.h>
#include <mios/task.h>

#include <net/ether.h>
#include <net/http/http.h>
#include <net/http/websocket.h>

#include "hosttest.h"
#include "sim.h"
#include "sim_tcp.h"

#define MSEC 1000ull
#define SEC  1000000ull

#define ROUNDS 3


// ---- Mios side ----

static volatile int gets;
static volatile int ws_opens;
static volatile int ws_disconnects;

// The connection the "keep" route holds a reference to, dropped by the
// suite's main thread once the connection is gone
static http_connection_t *volatile kept;
static volatile int kept_disconnected;

static int
ws_cb(void *opaque, int opcode, void *data, size_t size,
      http_connection_t *hc, balloc_t *ba)
{
  switch(opcode) {
  case WS_OPCODE_DISCONNECT:
    ws_disconnects++;
    if(opaque)
      kept_disconnected = 1;
    break;
  case WS_OPCODE_TEXT: {
    struct iovec iov = { data, size };
    http_websocket_sendv(hc, WS_OPCODE_TEXT, &iov, 1, 0);
    break;
  }
  }
  return 0;
}

static int
route_hello(http_request_t *hr, int argc, const char **argv)
{
  gets++;
  stream_t *st = http_response_begin(hr, 200, "text/plain");
  stprintf(st, "hello");
  stream_close(st);
  return 0;
}

HTTP_ROUTE_DEF("httptest/hello", route_hello);

// The server calls a websocket's cb with WS_OPCODE_OPEN only for one it
// opened itself (http_websocket_start()), so the routes count the opens
static int
route_ws(http_request_t *hr, int argc, const char **argv)
{
  int r = http_request_accept_websocket(hr, ws_cb, NULL, NULL, NULL);
  if(r == 0)
    ws_opens++;
  return r;
}

HTTP_ROUTE_DEF("httptest/ws", route_ws);

static int
route_keep(http_request_t *hr, int argc, const char **argv)
{
  http_connection_t *hc;
  int r = http_request_accept_websocket(hr, ws_cb, (void *)1, &hc, NULL);
  if(r == 0) {
    ws_opens++;
    kept = hc;
  }
  return r;
}

HTTP_ROUTE_DEF("httptest/keep", route_keep);


// ---- The client (simulation thread) ----

static vnet_t *http_vnet;
static uint32_t peer_ip, mios_ip;
static volatile int script_done;

typedef struct conn {
  sim_tcp_t t;
  uint32_t snd;        // Our next sequence number (relative)
  uint32_t rcv;        // Mios's next one
  uint8_t buf[4096];   // What Mios sent, in order
  size_t len;
  int fin, rst;
} conn_t;

static conn_t g_conn;

static int
conn_open(conn_t *c)
{
  static int next_port = 41000;
  memset(c, 0, sizeof(*c));
  const uint8_t mac[6] = { 0x02, 0xd0, 0x00, 0x00, 0x0a, 0x01 };
  memcpy(c->t.mac, mac, 6);
  c->t.vnet = http_vnet;
  c->t.ip = peer_ip;
  c->t.mios_ip = mios_ip;
  c->t.port = next_port++;
  c->t.mios_port = 80;
  c->t.mss = 1460;
  c->t.wscale = -1;
  c->t.isn = 7000000 + 1000 * next_port;
  return sim_tcp_connect(&c->t, 65535, NULL);
}

static void
conn_send(conn_t *c, const void *data, size_t len)
{
  sim_tcp_send(&c->t, SIM_TCP_ACK | SIM_TCP_PSH, c->snd, c->rcv, 65535,
               data, len);
  c->snd += len;
}

// Take in what Mios sends until 'done' says so (or it closes or resets),
// acknowledging it. 0 if 'timeout' passes with nothing from Mios
static int
conn_recv(conn_t *c, int (*done)(conn_t *c, void *arg), void *arg,
          uint64_t timeout)
{
  sim_tcp_seg_t seg;
  while(!done(c, arg)) {
    if(c->fin || c->rst)
      return 1;
    if(!sim_tcp_recv(&c->t, &seg, timeout))
      return 0;
    if(seg.flags & SIM_TCP_RST) {
      c->rst = 1;
      continue;
    }
    int ack = 0;
    if(seg.len && seg.seq == c->rcv && c->len + seg.len <= sizeof(c->buf)) {
      memcpy(c->buf + c->len, seg.data, seg.len);
      c->len += seg.len;
      c->rcv += seg.len;
      ack = 1;
    }
    if((seg.flags & SIM_TCP_FIN) && seg.seq + seg.len == c->rcv) {
      c->rcv++;
      c->fin = 1;
      ack = 1;
    }
    if(ack)
      sim_tcp_send(&c->t, SIM_TCP_ACK, c->snd, c->rcv, 65535, NULL, 0);
  }
  return 1;
}

static int
has_text(conn_t *c, void *arg)
{
  const char *s = arg;
  const size_t n = strlen(s);
  for(size_t i = 0; i + n <= c->len; i++)
    if(!memcmp(c->buf + i, s, n))
      return 1;
  return 0;
}

static int
has_bytes(conn_t *c, void *arg)
{
  return c->len >= (size_t)(intptr_t)arg;
}

static int
never(conn_t *c, void *arg)
{
  return 0;
}

// Drop what has been looked at
static void
conn_consume(conn_t *c, size_t n)
{
  memmove(c->buf, c->buf + n, c->len - n);
  c->len -= n;
}

// Our FIN, then wait for Mios's
static int
conn_close(conn_t *c)
{
  sim_tcp_send(&c->t, SIM_TCP_FIN | SIM_TCP_ACK, c->snd, c->rcv, 65535,
               NULL, 0);
  c->snd++;
  conn_recv(c, never, NULL, 2 * SEC);
  return c->fin || c->rst;
}

static void
conn_reset(conn_t *c)
{
  sim_tcp_send(&c->t, SIM_TCP_RST | SIM_TCP_ACK, c->snd, c->rcv, 0, NULL, 0);
}

// Let virtual time pass until *v reaches 'want'
static int
wait_for(volatile int *v, int want, uint64_t timeout)
{
  const uint64_t deadline = clock_get() + timeout;
  while(*v < want) {
    if(clock_get() >= deadline)
      return 0;
    sim_wait(clock_get() + MSEC);
  }
  return 1;
}

// A client's websocket frame: masked, as RFC 6455 has it
static void
ws_send(conn_t *c, int opcode, const void *data, size_t len)
{
  uint8_t f[2 + 4 + 125];
  const uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
  f[0] = 0x80 | opcode;
  f[1] = 0x80 | len;
  memcpy(f + 2, mask, 4);
  for(size_t i = 0; i < len; i++)
    f[6 + i] = ((const uint8_t *)data)[i] ^ mask[i & 3];
  conn_send(c, f, 6 + len);
}

// The next frame from Mios (unmasked, under 126 bytes): its opcode, or -1
static int
ws_recv(conn_t *c, char *payload, size_t *len, uint64_t timeout)
{
  if(!conn_recv(c, has_bytes, (void *)2, timeout) || c->len < 2)
    return -1;
  const size_t n = c->buf[1] & 0x7f;
  if(!conn_recv(c, has_bytes, (void *)(intptr_t)(2 + n), timeout) ||
     c->len < 2 + n)
    return -1;
  const int opcode = c->buf[0] & 0x0f;
  if(payload) {
    memcpy(payload, c->buf + 2, n);
    payload[n] = 0;
  }
  if(len)
    *len = n;
  conn_consume(c, 2 + n);
  return opcode;
}

static const char ws_request[] =
  "GET %s HTTP/1.1\r\n"
  "Host: mios\r\n"
  "Upgrade: websocket\r\n"
  "Connection: Upgrade\r\n"
  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
  "Sec-WebSocket-Version: 13\r\n"
  "\r\n";

// Connect, upgrade and see an echo. 1 on success
static int
ws_open(conn_t *c, const char *path)
{
  char req[256];
  if(!CHECK(conn_open(c), "%s: no TCP connection", path))
    return 0;
  const int n = snprintf(req, sizeof(req), ws_request, path);
  conn_send(c, req, n);
  conn_recv(c, has_text, "\r\n\r\n", 2 * SEC);
  if(!CHECK(has_text(c, "HTTP/1.1 101"), "%s: no 101", path))
    return 0;
  // RFC 6455's example key and its answer
  CHECK(has_text(c, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "%s: wrong accept",
        path);
  for(size_t i = 0; i + 4 <= c->len; i++) {
    if(!memcmp(c->buf + i, "\r\n\r\n", 4)) {
      conn_consume(c, i + 4);
      break;
    }
  }

  ws_send(c, WS_OPCODE_TEXT, "ping?", 5);
  char echo[126];
  const int op = ws_recv(c, echo, NULL, 2 * SEC);
  return CHECK(op == WS_OPCODE_TEXT && !strcmp(echo, "ping?"),
               "%s: no echo (opcode %d)", path, op);
}


// The server's idle timer ends the connection
static void
scn_get_server_closes(void)
{
  for(int i = 0; i < ROUNDS; i++) {
    const int before = gets;
    conn_t *c = &g_conn;
    if(!CHECK(conn_open(c), "GET: no TCP connection"))
      return;
    static const char req[] =
      "GET /httptest/hello HTTP/1.1\r\nHost: mios\r\n"
      "Connection: close\r\n\r\n";
    conn_send(c, req, sizeof(req) - 1);
    conn_recv(c, has_text, "hello", 2 * SEC);
    CHECK(has_text(c, "HTTP/1.1 200") && has_text(c, "hello"),
          "GET: no response");
    CHECK(has_text(c, "Connection: close"), "GET: no Connection: close");
    conn_recv(c, never, NULL, 10 * SEC);
    CHECK(c->fin, "GET with Connection: close: server did not close");
    conn_close(c);
    CHECK(gets == before + 1, "GET: route ran %d times", gets - before);
  }
}

static void
scn_get_client_closes(void)
{
  for(int i = 0; i < ROUNDS; i++) {
    conn_t *c = &g_conn;
    if(!CHECK(conn_open(c), "keep-alive GET: no TCP connection"))
      return;
    static const char req[] =
      "GET /httptest/hello HTTP/1.1\r\nHost: mios\r\n\r\n";
    conn_send(c, req, sizeof(req) - 1);
    conn_recv(c, has_text, "hello", 2 * SEC);
    CHECK(has_text(c, "hello"), "keep-alive GET: no response");
    CHECK(!c->fin, "keep-alive GET: server closed");
    CHECK(conn_close(c), "keep-alive GET: server did not close after us");
  }
}

static void
scn_ws_close_frame(void)
{
  for(int i = 0; i < ROUNDS; i++) {
    const int before = ws_disconnects;
    conn_t *c = &g_conn;
    if(!ws_open(c, "/httptest/ws"))
      return;
    const uint8_t status[2] = { 0x03, 0xe8 }; // 1000
    ws_send(c, WS_OPCODE_CLOSE, status, 2);
    const int op = ws_recv(c, NULL, NULL, 2 * SEC);
    CHECK(op == WS_OPCODE_CLOSE, "ws CLOSE: answered with opcode %d", op);
    conn_close(c);
    CHECK(wait_for(&ws_disconnects, before + 1, 2 * SEC),
          "ws CLOSE: no disconnect");
  }
}

static void
scn_ws_fin(void)
{
  for(int i = 0; i < ROUNDS; i++) {
    const int before = ws_disconnects;
    conn_t *c = &g_conn;
    if(!ws_open(c, "/httptest/ws"))
      return;
    conn_close(c);
    CHECK(wait_for(&ws_disconnects, before + 1, 2 * SEC),
          "ws FIN: no disconnect");
  }
}

static void
scn_ws_rst(void)
{
  for(int i = 0; i < ROUNDS; i++) {
    const int before = ws_disconnects;
    conn_t *c = &g_conn;
    if(!ws_open(c, "/httptest/ws"))
      return;
    conn_reset(c);
    CHECK(wait_for(&ws_disconnects, before + 1, 2 * SEC),
          "ws RST: no disconnect");
  }
}

// The client goes quiet: the server pings it (http_timer_cb()) and gives
// up after three unanswered
static void
scn_ws_ping_timeout(void)
{
  const int before = ws_disconnects;
  conn_t *c = &g_conn;
  if(!ws_open(c, "/httptest/ws"))
    return;
  int pings = 0;
  while(1) {
    const int op = ws_recv(c, NULL, NULL, 30 * SEC);
    if(op != WS_OPCODE_PING)
      break;
    pings++;
  }
  CHECK(pings == 3, "ws ping timeout: %d pings, want 3", pings);
  CHECK(wait_for(&ws_disconnects, before + 1, 10 * SEC),
        "ws ping timeout: no disconnect");
  CHECK(c->fin || c->rst, "ws ping timeout: connection still open");
  if(c->fin && !c->rst)
    conn_close(c);
}

// A route that keeps a reference: the server drops its own at the
// disconnect, the route's goes later (the suite's main thread)
static void
scn_ws_kept_reference(void)
{
  for(int i = 0; i < ROUNDS; i++) {
    const int before = ws_disconnects;
    kept_disconnected = 0;
    conn_t *c = &g_conn;
    if(!ws_open(c, "/httptest/keep"))
      return;
    conn_close(c);
    CHECK(wait_for(&ws_disconnects, before + 1, 2 * SEC),
          "kept ws: no disconnect");
    // The main thread releases it
    const uint64_t deadline = clock_get() + 2 * SEC;
    while(kept != NULL && clock_get() < deadline)
      sim_wait(clock_get() + MSEC);
    CHECK(kept == NULL, "kept ws: the route's reference was not released");
  }
}


static void
script_thread(void *arg)
{
  scn_get_server_closes();
  scn_get_client_closes();
  scn_ws_close_frame();
  scn_ws_fin();
  scn_ws_rst();
  scn_ws_ping_timeout();
  scn_ws_kept_reference();
  script_done = 1;
  while(1)
    sim_wait(SIM_NEVER);
}


// ---- The suite (Mios main thread) ----

static uint32_t
ip4(int a, int b, int c, int d)
{
  const uint8_t v[4] = { a, b, c, d };
  uint32_t r;
  memcpy(&r, v, 4);
  return r;
}

static int
run_http(void)
{
  // A static address and no link-up event, so no DHCP client
  const uint8_t mac[6] = { 0x02, 0xc0, 0x00, 0x00, 0x0a, 0x02 };
  http_vnet = vnet_create("veth10", mac);
  netif_t *ni = &vnet_eni(http_vnet)->eni_ni;
  mios_ip = ip4(10, 10, 0, 2);
  peer_ip = ip4(10, 10, 0, 1);
  ni->ni_ipv4_local_addr = mios_ip;
  ni->ni_ipv4_local_prefixlen = 24;

  sim_thread_create("http-script", script_thread, NULL, 0);

  // Release the "keep" route's reference once its connection is gone
  const uint64_t deadline = clock_get() + 600 * SEC;
  while(!script_done && clock_get() < deadline) {
    if(kept != NULL && kept_disconnected) {
      http_connection_t *hc = kept;
      kept_disconnected = 0;
      kept = NULL;
      http_connection_release(hc);
    }
    usleep(1000);
  }
  CHECK(script_done, "script did not finish");
  CHECK(ws_opens == ws_disconnects, "%d websockets opened, %d disconnected",
        ws_opens, ws_disconnects);
  CHECK(!sim_tcp_bad_cksum, "%d segments from Mios had a bad checksum",
        sim_tcp_bad_cksum);
  return 0; // failures are counted by CHECK()
}

HOSTTEST_SUITE("http", run_http, 0);
