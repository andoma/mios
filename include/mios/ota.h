#pragma once

#include <mios/error.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint32_t blocks;
  uint32_t crc;
  uint8_t hostaddr;
  char type;  // See OTA_TYPE_... defines
} ota_req_t;

error_t rpc_ota(const ota_req_t *in, void *out, size_t in_size);

error_t rpc_otamode(const void *in, uint8_t *out, size_t in_size);


#define OTA_TYPE_RAW      'r'
#define OTA_TYPE_SECTIONS 's'
