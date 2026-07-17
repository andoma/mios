#include <string.h>

#include "nrf54l_reg.h"
#include "nrf54l_trng.h"

// CRACEN wrapper: clock/power gating for the crypto engine blocks.
#define CRACEN_BASE       0x50048000
#define CRACEN_ENABLE     (CRACEN_BASE + 0x400)
#define CRACEN_ENABLE_RNG (1 << 1)

// The NIST/AIS31-style TRNG inside CRACENCORE: ring-oscillator noise source
// feeding a conditioning function and a 16-word output FIFO.
#define RNG_BASE          0x51801000  // CRACENCORE + 0x1000 (RNGCONTROL)
#define RNG_CONTROL       (RNG_BASE + 0x000)
#define RNG_FIFOLEVEL     (RNG_BASE + 0x004)  // 32-bit words available
#define RNG_STATUS        (RNG_BASE + 0x030)
#define RNG_FIFO          (RNG_BASE + 0x080)

#define RNG_CONTROL_ENABLE  (1 << 0)
#define RNG_CONTROL_SOFTRST (1 << 8)

#define RNG_STATUS_STATE(v)  (((v) >> 1) & 7)
#define RNG_STATE_ERROR      5

static void
trng_start(void)
{
  reg_or(CRACEN_ENABLE, CRACEN_ENABLE_RNG);

  // Reset pulse, then enable. Preserve the reset-value config bits in
  // CONTROL (sampling/conditioning setup) rather than clearing them.
  uint32_t ctrl = reg_rd(RNG_CONTROL) & ~RNG_CONTROL_ENABLE;
  reg_wr(RNG_CONTROL, ctrl | RNG_CONTROL_SOFTRST);
  reg_wr(RNG_CONTROL, ctrl & ~RNG_CONTROL_SOFTRST);
  reg_wr(RNG_CONTROL, ctrl | RNG_CONTROL_ENABLE);
}

void
nrf54l_trng_init(void)
{
  trng_start();
}

void
nrf54l_trng_read(uint8_t *buf, size_t len)
{
  while(len) {
    if(RNG_STATUS_STATE(reg_rd(RNG_STATUS)) == RNG_STATE_ERROR) {
      // Health test tripped (NIST 800-90B repetition/proportion): restart
      // the noise source and continue.
      trng_start();
      continue;
    }
    if(reg_rd(RNG_FIFOLEVEL) == 0)
      continue;
    const uint32_t w = reg_rd(RNG_FIFO);
    const size_t n = len < 4 ? len : 4;
    memcpy(buf, &w, n);
    buf += n;
    len -= n;
  }
}
