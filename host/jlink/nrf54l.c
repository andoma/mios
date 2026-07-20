#include "nrf54l.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Cortex-M33 debug registers
#define DHCSR 0xe000edf0
#define DEMCR 0xe000edfc
#define AIRCR 0xe000ed0c

#define DHCSR_DBGKEY     0xa05f0000
#define DHCSR_C_DEBUGEN  (1u << 0)
#define DHCSR_C_HALT     (1u << 1)
#define DHCSR_S_HALT     (1u << 17)

#define DEMCR_VC_CORERESET (1u << 0)

#define AIRCR_SYSRESETREQ 0x05fa0004

// FICR
#define FICR_INFO_PART    0x00ffc31c
#define FICR_INFO_VARIANT 0x00ffc320

// RRAMC (secure address)
#define RRAMC_BASE                 0x5004b000
#define RRAMC_TASKS_COMMITWRITEBUF (RRAMC_BASE + 0x008)
#define RRAMC_EVENTS_ACCESSERROR   (RRAMC_BASE + 0x10c)
#define RRAMC_READY                (RRAMC_BASE + 0x400)
#define RRAMC_ACCESSERRORADDR      (RRAMC_BASE + 0x408)
#define RRAMC_CONFIG               (RRAMC_BASE + 0x500)
#define RRAMC_ERASE_ERASEALL       (RRAMC_BASE + 0x540)

#define RRAMC_CONFIG_WEN          1
#define RRAMC_CONFIG_WRITEBUFSIZE (32 << 8)

// CTRL-AP (APSEL 2)
#define CTRLAP_APSEL 2

#define CTRLAP_RESET          0x00
#define CTRLAP_ERASEALL       0x04
#define CTRLAP_ERASEALLSTATUS 0x08

#define CTRLAP_RESET_NONE 0
#define CTRLAP_RESET_HARD 2
#define CTRLAP_RESET_PIN  4

#define CTRLAP_ERASEALLSTATUS_READY_TO_RESET 1
#define CTRLAP_ERASEALLSTATUS_BUSY           2

int
nrf54l_identify(dap_t *d, char *buf, size_t buflen)
{
  uint32_t part, variant;
  if(dap_mem_read32(d, FICR_INFO_PART, &part) ||
     dap_mem_read32(d, FICR_INFO_VARIANT, &variant))
    return -1;

  const char v[5] = {variant >> 24, variant >> 16, variant >> 8, variant, 0};
  snprintf(buf, buflen, "nRF%x %s", part, v);
  return part == 0x54b15 ? 0 : 1;
}

int
nrf54l_reset_halt(dap_t *d)
{
  if(dap_mem_write32(d, DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT))
    return -1;
  if(dap_mem_write32(d, DEMCR, DEMCR_VC_CORERESET))
    return -1;
  if(dap_mem_write32(d, AIRCR, AIRCR_SYSRESETREQ))
    return -1;

  for(int i = 0; ; i++) {
    uint32_t v;
    usleep(10000);
    if(dap_mem_read32(d, DHCSR, &v))
      return -1;
    if(v & DHCSR_S_HALT)
      break;
    if(i == 100)
      return -1;
  }
  // Clear reset vector catch (leave core halted)
  return dap_mem_write32(d, DEMCR, 0);
}

static int
rramc_wait_ready(dap_t *d)
{
  for(int i = 0; ; i++) {
    uint32_t v;
    if(dap_mem_read32(d, RRAMC_READY, &v))
      return -1;
    if(v & 1)
      return 0;
    if(i == 1000)
      return -1;
    usleep(1000);
  }
}

int
nrf54l_program(dap_t *d, uint32_t addr, const void *data, size_t len)
{
  if(dap_mem_write32(d, RRAMC_EVENTS_ACCESSERROR, 0))
    return -1;
  if(dap_mem_write32(d, RRAMC_CONFIG,
                     RRAMC_CONFIG_WEN | RRAMC_CONFIG_WRITEBUFSIZE))
    return -1;
  if(rramc_wait_ready(d))
    return -1;

  int r = dap_mem_write_block(d, addr, data, len);

  if(r == 0) {
    r = dap_mem_write32(d, RRAMC_TASKS_COMMITWRITEBUF, 1);
    if(r == 0)
      r = rramc_wait_ready(d);
  }

  uint32_t accerr = 0;
  if(r == 0) {
    r = dap_mem_read32(d, RRAMC_EVENTS_ACCESSERROR, &accerr);
    if(r == 0 && accerr) {
      uint32_t eaddr = 0;
      dap_mem_read32(d, RRAMC_ACCESSERRORADDR, &eaddr);
      fprintf(stderr, "RRAM access error at 0x%08x\n", eaddr);
      r = -1;
    }
  }

  // Disable write access again
  dap_mem_write32(d, RRAMC_CONFIG, 0);
  rramc_wait_ready(d);
  return r;
}

int
nrf54l_reset_run(dap_t *d)
{
  if(dap_mem_write32(d, DEMCR, 0))
    return -1;
  // Release the core from debug state, then reset
  if(dap_mem_write32(d, DHCSR, DHCSR_DBGKEY))
    return -1;
  return dap_mem_write32(d, AIRCR, AIRCR_SYSRESETREQ);
}

int
nrf54l_recover(dap_t *d)
{
  if(dap_ap_write(d, CTRLAP_APSEL, CTRLAP_ERASEALL, 1))
    return -1;

  for(int i = 0; ; i++) {
    uint32_t st;
    usleep(10000);
    if(dap_ap_read(d, CTRLAP_APSEL, CTRLAP_ERASEALLSTATUS, &st))
      return -1;
    if(st == CTRLAP_ERASEALLSTATUS_BUSY)
      continue;
    if(st == CTRLAP_ERASEALLSTATUS_READY_TO_RESET)
      break;
    if(i == 1000)
      return -1;
  }

  if(dap_ap_write(d, CTRLAP_APSEL, CTRLAP_RESET, CTRLAP_RESET_HARD))
    return -1;
  return dap_ap_write(d, CTRLAP_APSEL, CTRLAP_RESET, CTRLAP_RESET_NONE);
}
