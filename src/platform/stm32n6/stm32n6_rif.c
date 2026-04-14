#include "stm32n6_rif.h"
#include "stm32n6_clk.h"

#include <mios/eventlog.h>

#include <stdio.h>

#include "irq.h"

// IAC (Illegal Access Controller): nonsecure base
#define IAC_BASE   0x44025000
#define IAC_IER(n) (IAC_BASE + 0x00 + (n) * 4)
#define IAC_ISR(n) (IAC_BASE + 0x80 + (n) * 4)
#define IAC_ICR(n) (IAC_BASE + 0x100 + (n) * 4)

// RISAF instances to scan on illegal access
static const struct {
  uint32_t base;
  const char *name;
} risaf_table[] = {
  { 0x54026000, "RISAF1" },
  { 0x54027000, "RISAF2-axiRAM0" },
  { 0x54028000, "RISAF3-axiRAM1" },
  { 0x54029000, "RISAF4-NPU0" },
  { 0x5402A000, "RISAF5-NPU1" },
  { 0x5402B000, "RISAF6-CPU_MST" },
  { 0x5402C000, "RISAF7-FLEXMEM" },
  { 0x5402D000, "RISAF8-CACHEAXI" },
  { 0x5402E000, "RISAF9-VENCRAM" },
  { 0x54030000, "RISAF11-XSPI1" },
  { 0x54031000, "RISAF12-XSPI2" },
  { 0x54032000, "RISAF13-XSPI3" },
  { 0x54035000, "RISAF21-AHBRAM0" },
  { 0x54036000, "RISAF22-AHBRAM1" },
  { 0x54037000, "RISAF23-BKPRAM" },
};

#define NUM_RISAFS (sizeof(risaf_table) / sizeof(risaf_table[0]))

static void
iac_irq(void *arg)
{
  // Scan all RISAFs for the offending one
  for(unsigned i = 0; i < NUM_RISAFS; i++) {
    uint32_t iasr = reg_rd(risaf_table[i].base + 0x08);
    if(!(iasr & 0x2))
      continue;

    uint32_t iaesr = reg_rd(risaf_table[i].base + 0x20);
    uint32_t iaddr = reg_rd(risaf_table[i].base + 0x24);

    // Clear RISAF flag and IAC flag
    reg_wr(risaf_table[i].base + 0x0C, 0x3);
    for(int j = 0; j < 5; j++) {
      uint32_t isr = reg_rd(IAC_ISR(j));
      if(isr)
        reg_wr(IAC_ICR(j), isr);
    }

    int cid  = iaesr & 7;
    int priv = (iaesr >> 4) & 1;
    int sec  = (iaesr >> 5) & 1;
    int rw   = (iaesr >> 7) & 1;

    panic("RIF: %s blocked %s @0x%08x CID=%d %s %s",
          risaf_table[i].name,
          rw ? "write" : "read",
          iaddr, cid,
          sec ? "sec" : "nsec",
          priv ? "priv" : "unpriv");
  }

  // No RISAF violation — some other IAC source. Clear and report.
  uint32_t isr_vals[5];
  for(int i = 0; i < 5; i++) {
    isr_vals[i] = reg_rd(IAC_ISR(i));
    if(isr_vals[i])
      reg_wr(IAC_ICR(i), isr_vals[i]);
  }

  panic("RIF: IAC ISR=%08x/%08x/%08x/%08x/%08x",
        isr_vals[0], isr_vals[1], isr_vals[2], isr_vals[3], isr_vals[4]);
}


static void __attribute__((constructor(130)))
stm32n6_rif_init(void)
{
  // Clear any pending IAC flags from boot
  for(int i = 0; i < 5; i++)
    reg_wr(IAC_ICR(i), 0xFFFFFFFF);

  // Enable all IAC interrupt sources
  for(int i = 0; i < 5; i++)
    reg_wr(IAC_IER(i), 0xFFFFFFFF);

  irq_enable_fn_arg(13, IRQ_LEVEL_SCHED, iac_irq, NULL);
}
