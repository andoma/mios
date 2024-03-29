#pragma once

#include <mios/error.h>
#include <stddef.h>

const void *stm32g0_p13n_get(size_t len);

error_t stm32g0_p13n_put(const void *data, size_t len);

error_t stm32g0_flash_erase_sector_ramcode(int sector) __attribute__((warn_unused_result));
