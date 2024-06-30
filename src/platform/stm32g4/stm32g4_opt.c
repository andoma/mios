#include "stm32g4_opt.h"

#include "stm32g4_flash.h"
#include "stm32g4_reg.h"

#include "irq.h"

static void
stm32g4_flash_unlock(void)
{
  reg_wr(FLASH_KEYR, 0x45670123);
  reg_wr(FLASH_KEYR, 0xcdef89ab);
}

static void
stm32g4_opt_unlock(void)
{
  reg_wr(FLASH_OPTKEYR, 0x08192A3B);
  reg_wr(FLASH_OPTKEYR, 0x4C5D6E7F);
}

static void
stm32g4_flash_lock(void)
{
  reg_set_bit(FLASH_CR, 31);
}

static void
stm32g4_flash_wait_ready(void)
{
  while(reg_get_bit(FLASH_SR, 16)) {}
}

void
stm32g4_opt_set_boot_pins(int nSWBOOT0, int nBOOT0)
{
  int q = irq_forbid(IRQ_LEVEL_ALL);
  stm32g4_flash_wait_ready();

  stm32g4_flash_unlock();
  stm32g4_opt_unlock();

  reg_set_bits(FLASH_OPTR, 26, 1, nSWBOOT0);
  reg_set_bits(FLASH_OPTR, 27, 1, nBOOT0);

  reg_set_bit(FLASH_CR, 17);
  stm32g4_flash_wait_ready();

  stm32g4_flash_lock();
  irq_permit(q);
}
