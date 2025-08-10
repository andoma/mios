#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct vllp vllp_t;

typedef struct vllp_channel vllp_channel_t;


// Same as error_t in mios
#define VLLP_ERR_TIMEOUT        -2
#define VLLP_ERR_BAD_STATE      -13
#define VLLP_ERR_NOT_FOUND      -17
#define VLLP_ERR_CRC_MISMATCH   -18
#define VLLP_ERR_MALFORMED      -19
#define VLLP_ERR_NO_MEMORY      -29

#define VLLP_FDCAN_ADAPTATION 0x1

vllp_t *vllp_create_client(int mtu, uint32_t flags, void *opaque,
                           void (*tx)(void *opaque, const void *data,
                                      size_t len),
                           void (*log)(void *opaque, int syslog_level,
                                       const char *msg));


typedef struct {
  int error;
  void (*rx)(void *opaque, const void *data, size_t length);
  void (*eof)(void *opaque, int error_code);
  void *opaque;
} open_channel_result_t;

vllp_t *vllp_create_server(int mtu, uint32_t flags, void *opaque,
                           void (*tx)(void *opaque, const void *data,
                                      size_t len),
                           void (*log)(void *opaque, int syslog_level,
                                       const char *msg),
                           open_channel_result_t (*open_channel)(void *opaque,
                                                                 const char *name,
                                                                 vllp_channel_t *vc));

void vllp_start(vllp_t *v);

void vllp_input(vllp_t *v, const void *data, size_t len);

#define VLLP_CHANNEL_NO_CRC32       0x1
#define VLLP_CHANNEL_RECONNECT      0x2

vllp_channel_t *vllp_channel_create(vllp_t *v, const char *name,
                                    uint32_t flags,
                                    void (*rx)(void *opaque,
                                               const void *data,
                                               size_t length),
                                    void (*eof)(void *opaque,
                                               int error_code),
                                    void *opaque);

void vllp_channel_start(vllp_channel_t *vc);

void vllp_channel_send(vllp_channel_t *vc, const void *data, size_t len);

int vllp_channel_read(vllp_channel_t *vc, void **data, size_t *lenp, long timeout_us);

void vllp_channel_close(vllp_channel_t *vc, int error_code, int wait);

void vllp_channel_release(vllp_channel_t *vc);

const char *vllp_strerror(int error);

uint32_t vllp_crc32(uint32_t crc, const void *data, size_t len);

void vllp_logf(vllp_t *v, int level, const char *fmt, ...);
