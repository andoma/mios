#pragma once

#include <mios/error.h>

#define FLASH_BASE 0x52002000

#define FLASH_ACR          (FLASH_BASE + 0x00)
#define FLASH_KEYR         (FLASH_BASE + 0x04)
#define FLASH_OPTKEYR      (FLASH_BASE + 0x08)

#define FLASH_OPTCR        (FLASH_BASE + 0x18)

#define FLASH_OPTSR2_CUR   (FLASH_BASE + 0x70)
#define FLASH_OPTSR2_PRG   (FLASH_BASE + 0x74)

error_t stm32h7_set_cpu_freq_boost(int on);
