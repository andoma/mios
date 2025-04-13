#include "stm32h7_flash.h"

#include <stdint.h>
#include <unistd.h>

#include "stm32h7_reg.h"


error_t
stm32h7_set_cpu_freq_boost(int on)
{
  uint64_t deadline;

  if(reg_get_bit(FLASH_OPTCR, 0)) {
    // OPTFLASH locked, write unlock sequence
    reg_wr(FLASH_OPTKEYR, 0x08192A3B);
    reg_wr(FLASH_OPTKEYR, 0x4c5d6e7f);

    deadline = clock_get() + 10000;
    while(reg_get_bit(FLASH_OPTCR, 0)) {
      if(clock_get() > deadline) {
        return ERR_WRITE_PROTECTED;
      }
    }
  }

  reg_set_bits(FLASH_OPTSR2_PRG, 2, 1, on);
  reg_set_bit(FLASH_OPTCR, 1);
  deadline = clock_get() + 100000;

  while(reg_get_bit(FLASH_OPTCR, 1)) {
    if(clock_get() > deadline) {
      reg_set_bit(FLASH_OPTCR, 0);
      return ERR_FLASH_TIMEOUT;
    }
  }
  reg_set_bit(FLASH_OPTCR, 0);
  return 0;
}
