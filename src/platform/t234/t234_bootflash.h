#pragma once

#include <mios/error.h>

struct block_iface;

error_t t234_bootflash_install(struct block_iface *bi);

error_t t234_bootflash_set_chain(struct block_iface *bi, int chain);
