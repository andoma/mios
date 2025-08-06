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

typedef struct service {

  const char *name;

  uint16_t ip_port;
  uint8_t ble_psm;

  error_t (*open_pushpull)(pushpull_t *p);

  error_t (*open_stream)(struct stream *s);

} service_t;

const service_t *service_find_by_name(const char *name);

const service_t *service_find_by_namelen(const char *name, size_t len);

const service_t *service_find_by_ble_psm(uint8_t psm);

const service_t *service_find_by_ip_port(uint16_t port);

error_t service_open_pushpull(const service_t *svc, pushpull_t *pp);

#define SERVICE_DEF_STREAM(name, port, open)  \
static const service_t MIOS_JOIN(servicedef, __LINE__) __attribute__ ((used, section("servicedef"))) = { name, .ip_port = port, .open_stream = open};


#define SERVICE_DEF_PUSHPULL(name, ip_port, ble_psm, open)  \
static const service_t MIOS_JOIN(servicedef, __LINE__) __attribute__ ((used, section("servicedef"))) = { name, ip_port, ble_psm, .open_pushpull = open};

