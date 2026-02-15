#include "t234ccplex_bootchain.h"

#include <stdio.h>

#include <mios/io.h>
#include <mios/block.h>
#include <mios/cli.h>
#include <mios/ghook.h>
#include <mios/eventlog.h>

#include <drivers/spiflash.h>

#include "t234ccplex_qspi.h"

#include "t234_bootflash.h"
#include "t234_bootinfo.h"


/*
 * Coldboot into chain A:
 *     chain_status=1      bootloader_status=0004ef1
 *   Reset without chain validation (into chain B):
 *     chain_status=13     bootloader_status=4004ef1
 * Coldboot into chain B:
 *     chain_status=12     bootloader_status=0004ef1
 *   Reset without chain validation (into chain A):
 *     chain_status=3      bootloader_status=4004ef1
 *
 */

#define BOOTLOADER_MAGIC 0x4ef1

static block_iface_t *bootflash;

static void __attribute__((constructor(5000)))
bootchain_init(void)
{
  spi_t *qspi = tegra_qspi_init();
  block_iface_t *bi = spiflash_create(qspi, GPIO_UNUSED);
  bootflash = block_create_subdivision(bi, 3); // 4k -> 512b

  uint32_t bootloader_status = reg_rd(SCRATCH_BOOTLOADER_REGISTER);
  if((bootloader_status & 0xffff) == BOOTLOADER_MAGIC) {
    // We started from ROM bootloader via QSPI flash
    uint32_t chain_status = reg_rd(SCRATCH_BOOT_CHAIN_REGISTER);
    int active_chain = (chain_status >> 4) & 1;
    printf("boot mode: Chain-%c [0x%08x 0x%08x]\n", active_chain + 'A',
           bootloader_status, chain_status);

    if(bootloader_status & (1 << 26)) {
      // Failed to boot the chain configured in flash, now we're on
      // the backup. Reprogram flash
      printf("Primary bootchain is corrupt, swapping to current ... ");
      t234_bootflash_set_chain(bi, active_chain);
      printf("Done\n");
    }

  } else {
    // Recovery boot via USB APX interface
    printf("boot mode: Recovery\n");
  }
}

error_t
bootchain_install_bootflash(void)
{
  return t234_bootflash_install(bootflash);
}

void
bootchain_mark_valid(void)
{
  uint32_t bootloader_status = reg_rd(SCRATCH_BOOTLOADER_REGISTER);
  if((bootloader_status & 0xffff) == BOOTLOADER_MAGIC) {
    uint32_t chain_status = reg_rd(SCRATCH_BOOT_CHAIN_REGISTER);
    int active_chain = (chain_status >> 4) & 1;
    if(chain_status & (1 << active_chain)) {
      chain_status &= ~(1 << active_chain);
      reg_wr(SCRATCH_BOOT_CHAIN_REGISTER, chain_status);
      evlog(LOG_INFO, "Bootchain %c marked OK", active_chain + 'A');
    }
  }
}



static void
bootchain_shutdown_hook(ghook_type_t type, const char *reason)
{
  if(type != GHOOK_SYSTEM_SHUTDOWN)
    return;

  // We are about to shutdown (either reset, or boot into Linux). If
  // it hasn't already been done, now is our last opportunity to clear
  // the current chain's dirty bit
  bootchain_mark_valid();
}

GHOOK(bootchain_shutdown_hook);


static error_t
cmd_bootflash_install(cli_t *cli, int argc, char **argv)
{
  return bootchain_install_bootflash();
}

CLI_CMD_DEF_EXT("bootflash_install", cmd_bootflash_install,
                NULL, "Install bootflash (qspi) from RCMBoot (APX) contents");

static error_t
cmd_bootflash_erase(cli_t *cli, int argc, char **argv)
{
  error_t err = block_erase(bootflash, 0, bootflash->num_blocks);
  if(!err)
    err = block_ctrl(bootflash, BLOCK_SYNC);
  return err;
}

CLI_CMD_DEF_EXT("bootflash_erase", cmd_bootflash_erase,
                NULL, "Erase bootflash (qspi)");

static error_t
cmd_bootflash_setchain(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  int chain;
  switch(argv[1][0]) {
  case 'a':
  case 'A':
    chain = 0;
    break;
  case 'b':
  case 'B':
    chain = 1;
    break;
  default:
    return ERR_INVALID_ARGS;
  }

  error_t err = t234_bootflash_set_chain(bootflash, chain);

  if(err)
    return err;

  reg_wr(SCRATCH_BOOT_CHAIN_REGISTER, chain << 4);
  return 0;
}

CLI_CMD_DEF_EXT("bootflash_setchain", cmd_bootflash_setchain,
                "a|b", "Select active bootchain");
