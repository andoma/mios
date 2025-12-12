#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct vllp vllp_t;

typedef struct vllp_channel vllp_channel_t;


#define VLLP_ERR_OK                     0
#define VLLP_ERR_NOT_IMPLEMENTED        -1
#define VLLP_ERR_TIMEOUT                -2
#define VLLP_ERR_OPERATION_FAILED       -3
#define VLLP_ERR_TX                     -4
#define VLLP_ERR_RX                     -5
#define VLLP_ERR_NOT_READY              -6
#define VLLP_ERR_NO_BUFFER              -7
#define VLLP_ERR_MTU_EXCEEDED           -8
#define VLLP_ERR_INVALID_ID             -9
#define VLLP_ERR_DMA_XFER               -10
#define VLLP_ERR_BUS_ERROR              -11
#define VLLP_ERR_ARBITRATION_LOST       -12
#define VLLP_ERR_BAD_STATE              -13
#define VLLP_ERR_INVALID_ADDRESS        -14
#define VLLP_ERR_NO_DEVICE              -15
#define VLLP_ERR_MISMATCH               -16
#define VLLP_ERR_NOT_FOUND              -17
#define VLLP_ERR_CHECKSUM_ERROR         -18
#define VLLP_ERR_MALFORMED              -19
#define VLLP_ERR_INVALID_RPC_ID         -20
#define VLLP_ERR_INVALID_RPC_ARGS       -21
#define VLLP_ERR_NOSPC                  -22
#define VLLP_ERR_INVALID_ARGS           -23
#define VLLP_ERR_INVALID_LENGTH         -24
#define VLLP_ERR_NOT_IDLE               -25
#define VLLP_ERR_BAD_CONFIG             -26
#define VLLP_ERR_FLASH_HW_ERROR         -27
#define VLLP_ERR_FLASH_TIMEOUT          -28
#define VLLP_ERR_NO_MEMORY              -29
#define VLLP_ERR_READ_PROTECTED         -30
#define VLLP_ERR_WRITE_PROTECTED        -31
#define VLLP_ERR_AGAIN                  -32
#define VLLP_ERR_NOT_CONNECTED          -33
#define VLLP_ERR_BAD_PKT_SIZE           -34
#define VLLP_ERR_EXIST                  -35
#define VLLP_ERR_CORRUPT                -36
#define VLLP_ERR_NOTDIR                 -37
#define VLLP_ERR_ISDIR                  -38
#define VLLP_ERR_NOTEMPTY               -39
#define VLLP_ERR_BADF                   -40
#define VLLP_ERR_FBIG                   -41
#define VLLP_ERR_INVALID_PARAMETER      -42
#define VLLP_ERR_NOATTR                 -43
#define VLLP_ERR_TOOLONG                -44
#define VLLP_ERR_IO                     -45
#define VLLP_ERR_FS                     -46
#define VLLP_ERR_DMA_FIFO               -47
#define VLLP_ERR_INTERRUPTED            -48
#define VLLP_ERR_QUEUE_FULL             -49
#define VLLP_ERR_NO_ROUTE               -50


#define VLLP_FDCAN_ADAPTATION 0x1

vllp_t *vllp_create_client(int mtu, int timeout, uint32_t flags, void *opaque,
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

vllp_t *vllp_create_server(int mtu, int timeout, uint32_t flags, void *opaque,
                           void (*tx)(void *opaque, const void *data,
                                      size_t len),
                           void (*log)(void *opaque, int syslog_level,
                                       const char *msg),
                           open_channel_result_t (*open_channel)(void *opaque,
                                                                 const char *name,
                                                                 vllp_channel_t *vc));

void vllp_start(vllp_t *v);

void vllp_input(vllp_t *v, const void *data, size_t len);

void vllp_destroy(vllp_t *v);

#define VLLP_CHANNEL_RECONNECT      0x2

vllp_channel_t *vllp_channel_create(vllp_t *v, const char *name,
                                    uint32_t flags,
                                    void (*rx)(void *opaque,
                                               const void *data,
                                               size_t length),
                                    void (*eof)(void *opaque,
                                               int error_code),
                                    void (*rdy)(void *opaque),
                                    void *opaque);

void vllp_channel_start(vllp_channel_t *vc);

void vllp_channel_send(vllp_channel_t *vc, const void *data, size_t len);

int vllp_channel_read(vllp_channel_t *vc, void **data, size_t *lenp, long timeout_us);

void vllp_channel_close(vllp_channel_t *vc, int error_code, int wait);

const char *vllp_strerror(int error);

uint32_t vllp_crc32(uint32_t crc, const void *data, size_t len);

void vllp_logf(vllp_t *v, int level, const char *fmt, ...);
