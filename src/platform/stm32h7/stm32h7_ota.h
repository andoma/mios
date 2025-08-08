#pragma once

#include <mios/error.h>

struct pushpull;
struct block_iface;

error_t stm32h7_ota_open(struct pushpull *pp, struct block_iface *bi);

