#include <stdlib.h>
#include <unistd.h>
#include <mios/mios.h>
#include <mios/cli.h>

#include "stm32f4_reg.h"

#include "systick.h"
#include "irq.h"

#define FLASH_BASE 0x40023c00

#define FLASH_KEYR    (FLASH_BASE + 0x04)
#define FLASH_IOTKEYR (FLASH_BASE + 0x08)
#define FLASH_SR      (FLASH_BASE + 0x0c)
#define FLASH_CR      (FLASH_BASE + 0x10)

static volatile unsigned int * const SYST_CSR = (unsigned int *)0xe000e010;

static inline void __attribute__((always_inline))
flash_wait_ready(void)
{
  // All interrupts are off, and we need to check if systick
  // wraps since a flash erase of a 128k sector takes as much
  // as 2 seconds

  extern uint64_t clock;
  while(reg_rd(FLASH_SR) & (1 << 16)) {
    if(*SYST_CSR & 0x10000) {
      clock += 1000000 / HZ;
    }
  }
}



static int  __attribute__((section("ramcode")))
cmd_erase(cli_t *cli, int argc, char **argv)
{
  if(argc < 2)
    return 0;

  int sector = atoi(argv[1]);
  int64_t t1 = clock_get();

  int q = irq_forbid(IRQ_LEVEL_ALL);

  reg_wr(FLASH_CR, 0x2 | (sector << 3));
  reg_set_bit(FLASH_CR, 16);
  flash_wait_ready();

  irq_permit(q);

  t1 = clock_get() - t1;

  cli_printf(cli, "done: %d\n", (int)t1);

  return 0;
}


CLI_CMD_DEF("erase", cmd_erase);



static int
cmd_flash_write(cli_t *cli, int argc, char **argv)
{
  if(argc < 3)
    return 0;

  const uint32_t addr = atoix(argv[1]);
  const uint32_t value = atoix(argv[2]);

  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }

  reg_wr(FLASH_CR, 0x1 | (2 << 8));

  *(uint32_t *)addr = value;
  return 0;
}

CLI_CMD_DEF("write", cmd_flash_write);


static int
cmd_flash_unlock(cli_t *cli, int argc, char **argv)
{
  reg_wr(FLASH_KEYR, 0x45670123);
  reg_wr(FLASH_KEYR, 0xCDEF89AB);
  return 0;
}

CLI_CMD_DEF("flash-unlock", cmd_flash_unlock);
