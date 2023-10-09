#pragma once

#include "mios.h"
#include "error.h"
#include "socket.h"

#include <stdint.h>
#include <stddef.h>

#define SERVICE_TYPE_STREAM 0
#define SERVICE_TYPE_DGRAM  1

typedef struct {
  uint16_t max_fragment_size;
  uint16_t preferred_offset;
} svc_pbuf_policy_t;

typedef struct service {

  const char *name;

  uint16_t ip_port;
  uint8_t ble_psm;
  uint8_t type;

  error_t (*open)(socket_t *s);

} service_t;

const service_t *service_find_by_name(const char *name);

const service_t *service_find_by_ble_psm(uint8_t psm);

const service_t *service_find_by_ip_port(uint16_t port);

#define SERVICE_DEF(name, ip_port, ble_psm, type, open) \
static const service_t MIOS_JOIN(servicedev, __LINE__) __attribute__ ((used, section("servicedef"))) = { name, ip_port, ble_psm, type, open};

