#pragma once

#define FLASH_BASE 0x40022000

#define FLASH_KEYR    (FLASH_BASE + 0x08)
#define FLASH_OPTKEYR (FLASH_BASE + 0x0c)
#define FLASH_SR      (FLASH_BASE + 0x10)
#define FLASH_CR      (FLASH_BASE + 0x14)
#define FLASH_OPTR    (FLASH_BASE + 0x20)
