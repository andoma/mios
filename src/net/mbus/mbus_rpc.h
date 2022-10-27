#pragma once

#include <stddef.h>
#include <stdint.h>
#include <mios/error.h>

struct pbuf;

struct pbuf *mbus_rpc(uint8_t remote_addr, const char *method,
                      const uint8_t *data,
                      size_t len, error_t *errp);

