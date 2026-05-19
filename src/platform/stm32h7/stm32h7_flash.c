#include "stm32h7_flash.h"

#include <stdint.h>
#include <unistd.h>

#include <mios/cli.h>

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


static error_t
cmd_show_options(cli_t *cli, int argc, char **argv)
{
  uint32_t optsr = reg_rd(FLASH_OPTSR_CUR);
  uint32_t optsr2 = reg_rd(FLASH_OPTSR2_CUR);

  cli_printf(cli, "FLASH_OPTSR_CUR  = 0x%08x\n", optsr);
  cli_printf(cli, "FLASH_OPTSR2_CUR = 0x%08x\n\n", optsr2);

  uint8_t rdp = (optsr >> 8) & 0xff;
  const char *rdp_desc;
  switch(rdp) {
  case 0xaa: rdp_desc = "Level 0 (no protection)"; break;
  case 0xcc: rdp_desc = "Level 2 (locked permanently)"; break;
  default:   rdp_desc = "Level 1 (read-protected)"; break;
  }
  cli_printf(cli, "  RDP            0x%02x  %s\n", rdp, rdp_desc);

  static const char *bor_desc[8] = {
    "off", "Level 1 (~1.7 V)", "Level 2 (~2.1 V)", "Level 3 (~2.4 V)",
    "?", "?", "?", "?"
  };
  uint8_t bor = (optsr >> 16) & 0x7;
  cli_printf(cli, "  BOR_LEV        %d     %s\n", bor, bor_desc[bor]);

  cli_printf(cli, "  OPT_LOCK       %d     option-byte writes %s\n",
             (int)(optsr & 1),
             (optsr & 1) ? "locked" : "unlocked");
  cli_printf(cli, "  IWDG1_SW       %d     IWDG in %s mode\n",
             (int)((optsr >> 4) & 1),
             ((optsr >> 4) & 1) ? "software" : "hardware");
  cli_printf(cli, "  nRST_STOP      %d     reset on Stop %s\n",
             (int)((optsr >> 6) & 1),
             ((optsr >> 6) & 1) ? "disabled" : "enabled");
  cli_printf(cli, "  nRST_STBY      %d     reset on Standby %s\n",
             (int)((optsr >> 7) & 1),
             ((optsr >> 7) & 1) ? "disabled" : "enabled");
  cli_printf(cli, "  SECURITY       %d\n",
             (int)((optsr >> 21) & 1));
  cli_printf(cli, "  IWDG_FZ_STOP   %d     IWDG %s during Stop\n",
             (int)((optsr >> 24) & 1),
             ((optsr >> 24) & 1) ? "frozen" : "runs");
  cli_printf(cli, "  IWDG_FZ_SDBY   %d     IWDG %s during Standby\n",
             (int)((optsr >> 25) & 1),
             ((optsr >> 25) & 1) ? "frozen" : "runs");
  cli_printf(cli, "  IO_HSLV        %d\n",
             (int)((optsr >> 30) & 1));
  cli_printf(cli, "  SWAP_BANK_OPT  %d     %s\n",
             (int)((optsr >> 31) & 1),
             ((optsr >> 31) & 1) ? "banks swapped" : "default mapping");

  cli_printf(cli, "  CPUFREQ_BOOST  %d     %s\n",
             (int)((optsr2 >> 2) & 1),
             ((optsr2 >> 2) & 1) ? "off" : "on");
  return 0;
}

CLI_CMD_DEF_EXT("show_flash-options", cmd_show_options,
                NULL, "Show flash option bytes");
