#include "stm32g4_can.h"

#include "stm32g4_reg.h"
#include "stm32g4_clk.h"

#include <stdbool.h>

#define FDCAN_CREL   0x000
#define FDCAN_DBTP   0x00c
#define FDCAN_TEST   0x010
#define FDCAN_CCCR   0x018
#define FDCAN_NBTP   0x01c
#define FDCAN_ECR    0x040
#define FDCAN_PSR    0x044
#define FDCAN_IR     0x050
#define FDCAN_IE     0x054
#define FDCAN_ILS    0x058
#define FDCAN_ILE    0x05c

#define FDCAN_RXF0S  0x090
#define FDCAN_RXF0A  0x094
#define FDCAN_RXF1S  0x098
#define FDCAN_RXF1A  0x09c

#define FDCAN_TXFQS  0x0c4

#define FDCAN_TXBC   0x0c0
#define FDCAN_TXBAR  0x0cc

#define FDCAN_CKDIV  0x100

// The G4 FDCAN interrupt map is compressed relative to full M_CAN
// (no watermark bits): RF1N is bit 3 (not 4), BO is bit 19 (not 25),
// and ILS selects interrupt line per GROUP, not per flag.
#define FDCAN_IRQ_RF0N (1 << 0)
#define FDCAN_IRQ_RF1N (1 << 3)
#define FDCAN_IRQ_BO   (1 << 19)
#define FDCAN_ILS_RXFIFO1_TO_LINE1 (1 << 1)

#include "platform/stm32/stm32_fdcan.c"

#define FDCAN_BASE(x) (0x40006000 + ((x) * 0x400))

// Message RAM is one fixed section per instance, and unlike other M_CAN
// integrations the section addresses are not configurable: SIDFC, RXF0C,
// TXBC and friends do not select where the sections live here, so this
// has to match the hardware layout exactly. Sections start at 0x4000a400
// and are FDCAN_RAM_SIZE apart -- note that is 0x350, not the 0x400 that
// separates the register blocks. Deriving one stride from the other gives
// the right answer for instance 1 and silently wrong addresses for 2 and
// 3, where the driver then stages frames the core never reads and reads
// FIFO entries the core never wrote.
#define FDCAN_RAM_SIZE 0x350
#define FDCAN_RAM(x)  (0x4000a400 + (((x) - 1) * FDCAN_RAM_SIZE))

// Interrupt lines per instance (RM0440 NVIC table): IT0/IT1.
static const uint8_t fdcan_irq0[3] = { 21, 86, 88 };
static const uint8_t fdcan_irq1[3] = { 22, 87, 89 };

void
stm32g4_fdcan_init(int instance, gpio_t can_tx, gpio_t can_rx,
                   unsigned int nominal_bitrate,
                   unsigned int data_bitrate,
                   const struct dsig_filter *output_filter,
                   unsigned int flags)
{
  if(instance < 1 || instance > 3)
    panic("stm32g4_can: Invalid instance %d", instance);

  // One clock-enable and one reset bit are shared by all FDCAN
  // instances, so the reset must only happen once: doing it again for
  // a second instance would wipe the first one's configuration.
  static bool fdcan_clocked;
  if(!fdcan_clocked) {
    clk_enable(CLK_FDCAN);
    reset_peripheral(CLK_FDCAN);
    fdcan_clocked = true;
  }

  gpio_conf_af(can_tx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(can_rx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  fdcan_t *fc = calloc(1, sizeof(fdcan_t));
  fc->reg_base = FDCAN_BASE(instance);
  fc->ram_base = FDCAN_RAM(instance);

  stm32_fdcan_cce(fc, NULL);

  // The Bosch M_CAN User's Manual (section 1.3, Dual Clock Sources)
  // requires the Host clock (fdcan_pclk, i.e. APB1) to be >= the time
  // quantum clock for stable operation. With PLLQ feeding fdcan_ker_ck
  // at twice APB1, as the PLL setup here arranges, that is violated
  // unless something divides by two on the way to the CAN core.
  //
  // FDCAN_CKDIV divides fdcan_ker_ck before it reaches the CAN core, but
  // only FDCAN1 actually has one: it is not shared, and the other
  // instances have nothing writable at their own offset 0x100 (reads
  // back 0 after a write). Measured on hardware -- with FDCAN1's CKDIV
  // holding /2, FDCAN2 still ran the bus at exactly twice the configured
  // rate, because the bit timing had been computed for a divided clock
  // it never got.
  //
  // So instance 1 runs on the halved clock and the rest on the full
  // kernel clock, and the solver is told which. The wire rate comes out
  // identical either way; only the tq count differs. The M_CAN
  // requirement that the time-quantum clock not exceed the host clock is
  // handled by the pclk argument below, which forces a prescaler of 2 on
  // the undivided instances -- landing them on the same tq clock, and on
  // exact bit rates, without touching the APB prescalers.
  //
  // Protected write: FDCAN1's CCCR needs INIT+CCE, which the
  // stm32_fdcan_cce() above has established for instance 1 itself.
  uint32_t core_clk = clk_get_freq(CLK_FDCAN);
  if(instance == 1) {
    reg_wr(FDCAN_BASE(1) + FDCAN_CKDIV, 0b0001);
    core_clk /= 2;
  }

  for(size_t i = 0; i < FDCAN_RAM_SIZE; i += 4) {
    reg_wr(fc->ram_base + i, 0);
  }


  const char *name = "can";

  error_t err = stm32_fdcan_init(fc, name,
                                 nominal_bitrate,
                                 data_bitrate, core_clk,
                                 clk_get_freq(CLK_PCLK1),
                                 NULL,
                                 output_filter, flags);
  if(err) {
    printf("%s: Failed to initialize\n", name);
    return;
  }

  irq_enable_fn_arg(fdcan_irq0[instance - 1], IRQ_LEVEL_NET,
                    stm32_fdcan_irq0, fc);
  irq_enable_fn_arg(fdcan_irq1[instance - 1], IRQ_LEVEL_NET,
                    stm32_fdcan_irq1, fc);

  printf("%s: Initialized. Nominal bitrate:%d Data bitrate:%d\n",
         name, nominal_bitrate, data_bitrate);
}
