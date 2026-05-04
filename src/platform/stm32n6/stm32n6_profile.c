#include <mios/profile.h>
#include <mios/cli.h>

#include "stm32n6_clk.h"
#include "stm32n6_reg.h"
#include "stm32n6_tim.h"

#include "irq.h"

// TIM18 is a basic timer on APB2; TIM6 is already used by cecg2's pilot
// signal generator and TIM7 by mios systim, so neither is available here.
#define TIMP_BASE TIM18_BASE
#define TIMP_CLK  CLK_TIM18
#define TIMP_IRQ  135

// PCLK2 = 200 MHz, prescaler /200 → 1 MHz tick, ARR = 999 → 1 kHz
#define TIMP_PSC_VAL 199
#define TIMP_ARR_VAL 999

// CR1 bit layout: ARPE=0x80, OPM=0x8, URS=0x4, UDIS=0x2, CEN=0x1
#define TIM_CR1_ARPE  0x80
#define TIM_CR1_URS   0x04
#define TIM_CR1_CEN   0x01

// Naked ISR: read EXC_RETURN from lr to pick MSP/PSP, pull saved PC from
// frame[6], call profile_sample(), clear UIF, return. The dsb after the
// SR clear ensures the timer deasserts the IRQ line before exception
// return — otherwise NVIC could re-pend immediately.
__attribute__((naked))
static void
profile_isr(void)
{
  asm volatile(
    "tst lr, #4\n\t"
    "ite eq\n\t"
    "mrseq r0, msp\n\t"
    "mrsne r0, psp\n\t"
    "ldr r0, [r0, #24]\n\t"     // frame[6] = saved PC
    "push {lr}\n\t"
    "bl profile_sample\n\t"
    "ldr r0, =%[sr]\n\t"        // TIM18 status register
    "movs r1, #0\n\t"
    "str r1, [r0]\n\t"          // clear UIF
    "dsb\n\t"
    "pop {pc}\n\t"
    : : [sr] "i" (TIMP_BASE + TIMx_SR)
  );
}

static error_t
cmd_profile_start(cli_t *cli, int argc, char **argv)
{
  reg_wr(TIMP_BASE + TIMx_CNT, 0);
  reg_wr(TIMP_BASE + TIMx_SR, 0);
  reg_wr(TIMP_BASE + TIMx_CR1, TIM_CR1_ARPE | TIM_CR1_URS | TIM_CR1_CEN);
  return 0;
}

static error_t
cmd_profile_stop(cli_t *cli, int argc, char **argv)
{
  reg_wr(TIMP_BASE + TIMx_CR1, TIM_CR1_ARPE | TIM_CR1_URS);  // CEN=0
  return 0;
}

static void __attribute__((constructor(141)))
stm32n6_profile_init(void)
{
  clk_enable(TIMP_CLK);
  // URS=1 so the upcoming UG load doesn't trip an interrupt; ARPE=1 to
  // double-buffer ARR like the rest of the timer state.
  reg_wr(TIMP_BASE + TIMx_CR1, TIM_CR1_ARPE | TIM_CR1_URS);
  reg_wr(TIMP_BASE + TIMx_PSC, TIMP_PSC_VAL);
  reg_wr(TIMP_BASE + TIMx_ARR, TIMP_ARR_VAL);
  reg_wr(TIMP_BASE + TIMx_EGR, 0x1);       // UG: load PSC/ARR shadows
  reg_wr(TIMP_BASE + TIMx_SR, 0);          // defensive: clear any pending
  reg_wr(TIMP_BASE + TIMx_DIER, 0x1);      // UIE

  irq_enable_fn(TIMP_IRQ, IRQ_LEVEL_PROFILE, profile_isr);
}

static error_t
cmd_profile_status(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "TIM18 CR1 =0x%08x\n", reg_rd(TIMP_BASE + TIMx_CR1));
  cli_printf(cli, "      SR  =0x%08x\n", reg_rd(TIMP_BASE + TIMx_SR));
  cli_printf(cli, "      DIER=0x%08x\n", reg_rd(TIMP_BASE + TIMx_DIER));
  cli_printf(cli, "      PSC =%u\n",     reg_rd(TIMP_BASE + TIMx_PSC));
  cli_printf(cli, "      ARR =%u\n",     reg_rd(TIMP_BASE + TIMx_ARR));
  cli_printf(cli, "      CNT =%u\n",     reg_rd(TIMP_BASE + TIMx_CNT));
  return 0;
}

CLI_CMD_DEF("profile_start",  cmd_profile_start);
CLI_CMD_DEF("profile_stop",   cmd_profile_stop);
CLI_CMD_DEF("profile_status", cmd_profile_status);
