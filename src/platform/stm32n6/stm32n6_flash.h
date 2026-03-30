#pragma once

#include <mios/block.h>

enum {
  FLASH_PARTITION_FSBL1,
  FLASH_PARTITION_FSBL2,
  FLASH_PARTITION_BOOTSELECTOR,
  FLASH_PARTITION_APP_A,
  FLASH_PARTITION_APP_B,
  FLASH_PARTITION_FILESYSTEM,
  FLASH_PARTITION_COUNT,
};

block_iface_t *stm32n6_flash_get_partition(int partition);
