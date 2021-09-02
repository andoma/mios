#pragma once

// Persistent Key-Value store

#include <stddef.h>

#include "error.h"
#include "stream.h"

typedef struct pkv pkv_t;
struct flash_iface;

struct pkv *pkv_obtain_global(void);

struct pkv *pkv_create(const struct flash_iface *fif,
                       int sector_a, int sector_b);

error_t pkv_gc(struct pkv *pkv);

void pkv_show(struct pkv *pkv, stream_t *out);



error_t pkv_get(struct pkv *pkv, const char *key, void *buf, size_t *len);

int pkv_get_int(struct pkv *pkv, const char *key, int default_value);



error_t pkv_set(struct pkv *pkv, const char *key, const void *buf, size_t len);

error_t pkv_set_int(struct pkv *pkv, const char *key, int value);
