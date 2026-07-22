#include <mios/mios.h>

#include "irq.h"
#include "mpsl.h"

#include "nrf_sdc.h"
#include "nrf52_reg.h"

// Interrupt lines MPSL/SDC own on the nRF52 series. The time-critical
// handlers run at NVIC priority 0: mios irq_forbid() uses BASEPRI with levels
// >= 1, so priority 0 is never masked by kernel critical sections. The
// low-priority side (SWI5, POWER_CLOCK) runs at IRQ_LEVEL_NET; sharing one
// NVIC priority serializes them, which is what the SDC requires.
//
// mios uses RTC1 (systim), RTC2 (clock) and TIMER2 (mbus), so MPSL's RTC0 /
// TIMER0 / RADIO do not collide. POWER_CLOCK is shared: when BLE is built in,
// MPSL owns it (nrf52_clk.c stands down; see there).
#define POWER_CLOCK_IRQ  0
#define RADIO_IRQ        1
#define TIMER0_IRQ       8
#define RTC0_IRQ         11
#define SWI5_EGU5_IRQ    25

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
  NVIC_ISPR[SWI5_EGU5_IRQ >> 5] = 1 << (SWI5_EGU5_IRQ & 0x1f);
}

static void
mpsl_assert_handler(const char *file, uint32_t line)
{
  panic("mpsl assert %s:%d", file, line);
}

// Called by MPSL around time-critical radio work so the platform can switch
// flash latency / CPU power profile. We keep the CPU in a fixed profile, so
// these are no-ops.
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

  // The nRF52840 Dongle has no 32.768 kHz crystal, so run the LFCLK from the
  // internal RC oscillator and let MPSL calibrate it against the HFCLK. The
  // recommended RC config: calibrate every rc_ctiv*250 ms, and always after
  // rc_temp_ctiv intervals or a 0.5 C change.
  static const mpsl_clock_lfclk_cfg_t lfclk = {
    .source = MPSL_CLOCK_LF_SRC_RC,
    .rc_ctiv = 16,
    .rc_temp_ctiv = 2,
    .accuracy_ppm = 500,
  };

  irq_enable_fn(RADIO_IRQ, 0, MPSL_IRQ_RADIO_Handler);
  irq_enable_fn(TIMER0_IRQ, 0, MPSL_IRQ_TIMER0_Handler);
  irq_enable_fn(RTC0_IRQ, 0, MPSL_IRQ_RTC0_Handler);
  irq_enable_fn(POWER_CLOCK_IRQ, IRQ_LEVEL_NET, MPSL_IRQ_CLOCK_Handler);
  irq_enable_fn(SWI5_EGU5_IRQ, IRQ_LEVEL_NET, mpsl_low_prio_irq);

  int err = mpsl_init(&lfclk, SWI5_EGU5_IRQ, mpsl_assert_handler);
  if(err)
    panic("mpsl_init: %d", err);
}


// FICR factory device address (nRF52 Product Spec, FICR at 0x10000000).
#define FICR_DEVICEADDR0 0x100000a4
#define FICR_DEVICEADDR1 0x100000a8

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
