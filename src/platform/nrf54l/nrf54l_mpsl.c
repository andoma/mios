#include <mios/mios.h>

#include "irq.h"
#include "mpsl.h"

#include "nrf_sdc.h"
#include "nrf54l_reg.h"

// Interrupt lines used by MPSL/SDC on nRF54L15. The time-critical handlers
// run at NVIC priority 0: mios irq_forbid() uses BASEPRI with levels >= 1,
// so priority 0 is never masked by kernel critical sections (only by the
// panic path's cpsid). The low-priority side (SWI00, CLOCK_POWER) runs at
// IRQ_LEVEL_NET; sharing one NVIC priority means those handlers cannot
// preempt each other, which provides the serialization the SDC requires.
#define SWI00_IRQ        28
#define TIMER10_IRQ      133
#define RADIO_0_IRQ      138
#define GRTC_3_IRQ       229
#define CLOCK_POWER_IRQ  261

static void (*mpsl_low_prio_fn)(void);

static void
mpsl_low_prio_irq(void)
{
  mpsl_low_prio_fn();
}

void
nrf_mpsl_kick(void)
{
  static volatile unsigned int * const NVIC_ISPR = (unsigned int *)0xe000e200;
  NVIC_ISPR[SWI00_IRQ >> 5] = 1 << (SWI00_IRQ & 0x1f);
}

static void
mpsl_assert_handler(const char *file, uint32_t line)
{
  panic("mpsl assert %s:%d", file, line);
}

// Called by MPSL around time-critical radio work so the platform can switch
// NVM latency / CPU power profile. We never put the RRAM controller or CPU
// in a low-latency-violating power state, so these are no-ops.
void
mpsl_low_latency_acquire_callback(void)
{
}

void
mpsl_low_latency_release_callback(void)
{
}

void
nrf_mpsl_init(void (*low_prio)(void))
{
  mpsl_low_prio_fn = low_prio;

  // The DK's 32.768 kHz crystal. MPSL drives the CLOCK peripheral itself
  // (LFCLK start, HFXO requests); nrf54l_systim has already started the GRTC
  // SYSCOUNTER, which MPSL requires before mpsl_init().
  static const mpsl_clock_lfclk_cfg_t lfclk = {
    .source = MPSL_CLOCK_LF_SRC_XTAL,
    .accuracy_ppm = 50,
  };

  irq_enable_fn(RADIO_0_IRQ, 0, MPSL_IRQ_RADIO_Handler);
  irq_enable_fn(TIMER10_IRQ, 0, MPSL_IRQ_TIMER0_Handler);
  irq_enable_fn(GRTC_3_IRQ, 0, MPSL_IRQ_RTC0_Handler);
  irq_enable_fn(CLOCK_POWER_IRQ, IRQ_LEVEL_NET, MPSL_IRQ_CLOCK_Handler);
  irq_enable_fn(SWI00_IRQ, IRQ_LEVEL_NET, mpsl_low_prio_irq);

  int err = mpsl_init(&lfclk, SWI00_IRQ, mpsl_assert_handler);
  if(err)
    panic("mpsl_init: %d", err);
}


// FICR factory device address (same source the native link layer used).
#define FICR_DEVICEADDR0 0x00ffc3a4
#define FICR_DEVICEADDR1 0x00ffc3a8

void
nrf_ficr_ble_addr(uint8_t addr[6])
{
  const uint32_t da0 = reg_rd(FICR_DEVICEADDR0);
  const uint32_t da1 = reg_rd(FICR_DEVICEADDR1);
  addr[0] = da0;
  addr[1] = da0 >> 8;
  addr[2] = da0 >> 16;
  addr[3] = da0 >> 24;
  addr[4] = da1;
  addr[5] = (da1 >> 8) | 0xc0; // top two bits set: static random address
}
