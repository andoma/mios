#pragma once

#include "mios.h"
#include "error.h"

#include <mios/pushpull.h>

#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint16_t max_fragment_size;
  uint16_t preferred_offset;
} svc_pbuf_policy_t;

struct stream;

// Minimum BLE link security a service requires (GAP LE Security Mode 1). The
// value is an ordered level: the stack admits a connection only when the
// link's achieved level is >= the service's. Only NONE/ENCRYPTED are
// reachable today; AUTH/AUTH_SC become satisfiable when authenticated and
// LE Secure Connections pairing land, at which point a service demanding them
// is simply unopenable until then (rejected, never silently downgraded).
#define BLE_SEC_NONE      0  // L1: no encryption required
#define BLE_SEC_ENCRYPTED 1  // L2: encrypted (Just Works or better)
#define BLE_SEC_AUTH      2  // L3: authenticated / MITM-protected
#define BLE_SEC_AUTH_SC   3  // L4: authenticated + LE Secure Connections

typedef struct service {

  const char *name;

  uint16_t ip_port;
  uint8_t ble_psm;
  uint8_t ble_sec_level; // BLE_SEC_*, minimum link security

  error_t (*open_pushpull)(pushpull_t *p);

  error_t (*open_stream)(struct stream *s);

  // Per-service stream FIFO sizes, encoded as a bit shift (size = 1 <<
  // shift). The FIFO must be a power of two anyway, and 17 (128 KB) is
  // already past anything sensible on a microcontroller, so a uint8_t
  // covers the full range. Zero falls back to the protocol default —
  // small, suited to interactive shells / RPC traffic. Services that
  // stream high-bandwidth data should set these explicitly via
  // SERVICE_DEF_STREAM_EX.
  uint8_t txfifo_size_log2;
  uint8_t rxfifo_size_log2;

} service_t;

const service_t *service_find_by_name(const char *name);

const service_t *service_find_by_namelen(const char *name, size_t len);

const service_t *service_find_by_ble_psm(uint8_t psm);

const service_t *service_find_by_ip_port(uint16_t port);

error_t service_open_pushpull(const service_t *svc, pushpull_t *pp);

#define SERVICE_DEF_STREAM(name, port, open)  \
static const service_t MIOS_JOIN(servicedef, __LINE__) __attribute__ ((used, section("servicedef"))) = { name, .ip_port = port, .open_stream = open};

// Same as SERVICE_DEF_STREAM but with explicit per-service TX/RX FIFO
// sizes, given as base-2 logarithms (size = 1 << log2). Use for
// services whose traffic profile differs from the default (high-
// bandwidth streaming, large bulk transfers, etc).
#define SERVICE_DEF_STREAM_EX(name, port, txfifo_log2, rxfifo_log2, open) \
static const service_t MIOS_JOIN(servicedef, __LINE__) __attribute__ ((used, section("servicedef"))) = { name, .ip_port = port, .open_stream = open, .txfifo_size_log2 = (txfifo_log2), .rxfifo_size_log2 = (rxfifo_log2)};


#define SERVICE_DEF_PUSHPULL(name, ip_port, ble_psm, open)  \
static const service_t MIOS_JOIN(servicedef, __LINE__) __attribute__ ((used, section("servicedef"))) = { name, ip_port, ble_psm, .open_pushpull = open};

// Same as SERVICE_DEF_PUSHPULL but with a minimum BLE security level (BLE_SEC_*).
#define SERVICE_DEF_PUSHPULL_SEC(name, ip_port, ble_psm, sec, open)  \
static const service_t MIOS_JOIN(servicedef, __LINE__) __attribute__ ((used, section("servicedef"))) = { name, ip_port, ble_psm, .ble_sec_level = (sec), .open_pushpull = open};

