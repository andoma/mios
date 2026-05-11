/*
 * VLLP over DSIG. Stands up a host VLLP endpoint on top of a dsig_t bus
 * by tying host/vllp.c's tx callback to dsig_send() on a tx signal id
 * and feeding inbound frames on an rx signal id into vllp_input().
 *
 * The returned handle owns the underlying vllp_t; use dsig_vllp_get_vllp()
 * to layer services (vllp_channel_create, vllp_logstream_create, ...) on
 * top.
 *
 * Link with: host/dsig.c, host/dsig_vllp.c, host/vllp.c, -lpthread
 */

#pragma once

#include <stdint.h>

#include "dsig.h"
#include "vllp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsig_vllp dsig_vllp_t;

dsig_vllp_t *dsig_vllp_client_create(
    dsig_t *bus, uint32_t txid, uint32_t rxid,
    int mtu, int timeout, uint32_t vllp_flags,
    void *log_opaque,
    void (*log)(void *opaque, int syslog_level, const char *msg));

dsig_vllp_t *dsig_vllp_server_create(
    dsig_t *bus, uint32_t txid, uint32_t rxid,
    int mtu, int timeout, uint32_t vllp_flags,
    void *opaque,
    void (*log)(void *opaque, int syslog_level, const char *msg),
    open_channel_result_t (*open_channel)(void *opaque, const char *name,
                                          vllp_channel_t *vc));

vllp_t *dsig_vllp_get_vllp(dsig_vllp_t *dv);

void dsig_vllp_destroy(dsig_vllp_t *dv);

#ifdef __cplusplus
}
#endif
