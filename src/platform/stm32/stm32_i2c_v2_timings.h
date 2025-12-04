#include <stdint.h>
#include <limits.h>


/* Analog filter delay (AF) range is roughly 50..260ns when enabled. We'll
 * just use the worst case for safety. If you disable AF, set both to 0. */
#define I2C_AF_MIN_NS   50U
#define I2C_AF_MAX_NS   260U

#define I2C_PRESC_MAX   15U
#define I2C_SCLDEL_MAX  15U
#define I2C_SDADEL_MAX  15U
#define I2C_SCLH_MAX    255U
#define I2C_SCLL_MAX    255U

typedef struct {
    uint8_t  presc;
    uint8_t  scldel;
    uint8_t  sdadel;
    uint8_t  sclh;
    uint8_t  scll;
} i2c_timing_config_t;

static uint32_t
div_ceil_u64(uint64_t num, uint64_t den)
{
    return (uint32_t)((num + den - 1U) / den);
}

/*
 * Compute I2C timing according to AN4235 / RM formulas.
 *
 * Inputs:
 *   f_i2cclk_hz : I2C kernel clock (I2CCLK)
 *   f_scl_hz    : desired SCL frequency
 *   af_enable   : 1 = analog filter enabled, 0 = disabled
 *   dnf         : digital filter coefficient (0..15), tDNF = dnf * tI2CCLK
 *
 * Return -1 on failure
 * Otherwise the return value is I2C_TIMINGR
 */

__attribute__((const, always_inline))
static inline uint32_t stm32_i2c_calc_timingr(uint32_t f_i2cclk_hz,
                                              uint32_t f_scl_hz,
                                              int      af_enable,
                                              uint8_t  dnf)
{
  if (!f_i2cclk_hz || !f_scl_hz)
    return -1;

  uint32_t t_low_min_ns, t_high_min_ns;
  uint32_t t_su_dat_min_ns, t_hd_dat_min_ns, t_vd_dat_max_ns;
  uint32_t t_rise_ns;
  uint32_t t_fall_ns;

  if (f_scl_hz <= 100000U) {
    /* Standard mode */
    t_rise_ns         = 1000U;
    t_fall_ns         = 300U;
    t_low_min_ns      = 4700U;
    t_high_min_ns     = 4000U;
    t_su_dat_min_ns   = 250U;
    t_hd_dat_min_ns   = 0U;
    t_vd_dat_max_ns   = 3450U;
  } else if (f_scl_hz <= 400000U) {
    /* Fast mode */
    t_rise_ns = 300U;
    t_fall_ns = 300U;
    t_low_min_ns      = 1300U;
    t_high_min_ns     = 600U;
    t_su_dat_min_ns   = 100U;
    t_hd_dat_min_ns   = 0U;
    t_vd_dat_max_ns   = 900U;
  } else {
    /* Fast mode plus (up to 1 MHz) */
    t_rise_ns         = 120U;
    t_fall_ns         = 120U;
    t_low_min_ns      = 500U;
    t_high_min_ns     = 260U;
    t_su_dat_min_ns   = 50U;
    t_hd_dat_min_ns   = 0U;
    t_vd_dat_max_ns   = 450U;
  }

  const uint64_t ONE_BILLION = 1000000000ULL;

  uint64_t t_i2cclk_ns = ONE_BILLION / (uint64_t)f_i2cclk_hz;
  uint64_t t_scl_target_ns = ONE_BILLION / (uint64_t)f_scl_hz;

  uint64_t tAF_min_ns = (af_enable ? I2C_AF_MIN_NS : 0U);
  uint64_t tAF_max_ns = (af_enable ? I2C_AF_MAX_NS : 0U);
  uint64_t tDNF_ns    = (uint64_t)dnf * t_i2cclk_ns;

  int      best_valid = 0;
  uint64_t best_error = UINT64_MAX;
  i2c_timing_config_t best = {0};

  /* Scan PRESC 0..15 and find a combination which meets timing. */
  for (uint32_t presc = 0; presc <= I2C_PRESC_MAX; ++presc) {

    uint64_t t_presc_ns = (uint64_t)(presc + 1U) * t_i2cclk_ns;

    /* ----- SCLDEL (data setup time) -------------------------------
       SCLDEL >= ((tr + tSU;DAT(min)) / tPRESC) - 1
    */
    uint64_t num_scldel = (uint64_t)t_rise_ns + (uint64_t)t_su_dat_min_ns;
    uint32_t scldel = 0;
    if (t_presc_ns != 0U) {
      uint32_t scldel_min = 0;
      uint32_t tmp = div_ceil_u64(num_scldel, t_presc_ns);
      if (tmp > 0)
        scldel_min = tmp - 1U;
      if (scldel_min > I2C_SCLDEL_MAX)
        continue; /* impossible with this presc */

      scldel = scldel_min;
    }

    /* ----- SDADEL (data hold time) --------------------------------
       From AN4235:
       SDADEL >= {tf + tHD;DAT(min) - tAF(min) - tDNF - 3*tI2CCLK} / tPRESC
       SDADEL <= {tVD;DAT(max) - tr - tAF(max) - tDNF - 4*tI2CCLK} / tPRESC
    */
    int32_t  sda_min = 0, sda_max = (int32_t)I2C_SDADEL_MAX;

    {
      int64_t num_min =
        (int64_t)t_fall_ns +
        (int64_t)t_hd_dat_min_ns -
        (int64_t)tAF_min_ns -
        (int64_t)tDNF_ns -
        (int64_t)(3U * t_i2cclk_ns);

      int64_t num_max =
        (int64_t)t_vd_dat_max_ns -
        (int64_t)t_rise_ns -
        (int64_t)tAF_max_ns -
        (int64_t)tDNF_ns -
        (int64_t)(4U * t_i2cclk_ns);

      if (num_min < 0)
        num_min = 0; /* clamp, 0 is allowed */
      if (num_max < 0)
        continue;     /* no valid SDADEL */

      sda_min = (int32_t)div_ceil_u64((uint64_t)num_min, t_presc_ns);
      sda_max = (int32_t)((uint64_t)num_max / t_presc_ns);
      if (sda_min < 0)
        sda_min = 0;
      if (sda_max > (int32_t)I2C_SDADEL_MAX)
        sda_max = (int32_t)I2C_SDADEL_MAX;
      if (sda_min > sda_max)
        continue; /* no solution for this presc */
    }

    uint32_t sdadel = (uint32_t)sda_min; /* choose the smallest valid */

    /* ----- SCLH/SCLL (high/low periods) ---------------------------
       tHIGH(min) <= tAF(min) + tDNF + 2*tI2CCLK + (SCLH+1)*tPRESC
       tLOW(min)  <= tAF(min) + tDNF + 2*tI2CCLK + (SCLL+1)*tPRESC
       We start from the minimum and then add extra cycles to match f_scl.
    */

    uint64_t base_delay_ns = tAF_min_ns + tDNF_ns + 2U * t_i2cclk_ns;

    /* Minimum SCLH/SCLL values from the inequalities above */
    uint32_t sclh_min = 0, scll_min = 0;
    {
      int64_t num_high =
        (int64_t)t_high_min_ns - (int64_t)base_delay_ns;
      int64_t num_low =
        (int64_t)t_low_min_ns  - (int64_t)base_delay_ns;

      if (num_high < 0)
        num_high = 0;
      if (num_low < 0)
        num_low = 0;

      sclh_min = div_ceil_u64((uint64_t)num_high, t_presc_ns);
      scll_min = div_ceil_u64((uint64_t)num_low, t_presc_ns);

      if (sclh_min > I2C_SCLH_MAX || scll_min > I2C_SCLL_MAX)
        continue;
    }

    /* Compute actual tHIGH/tLOW from these mins */
    uint64_t t_high_ns = base_delay_ns +
      (uint64_t)(sclh_min + 1U) * t_presc_ns;
    uint64_t t_low_ns  = base_delay_ns +
      (uint64_t)(scll_min + 1U) * t_presc_ns;

    uint64_t t_sum_ns = (uint64_t)t_rise_ns + (uint64_t)t_fall_ns
      + t_high_ns + t_low_ns;

    /* If we're shorter than target period, add extra cycles evenly to H/L */
    if (t_sum_ns < t_scl_target_ns) {
      uint64_t delta_ns = t_scl_target_ns - t_sum_ns;
      uint32_t extra = (uint32_t)(delta_ns / t_presc_ns);

      /* distribute: more to low than high if odd */
      uint32_t extra_low  = extra / 2U + (extra % 2U);
      uint32_t extra_high = extra / 2U;

      if (scll_min + extra_low  > I2C_SCLL_MAX)
        extra_low  = I2C_SCLL_MAX - scll_min;
      if (sclh_min + extra_high > I2C_SCLH_MAX)
        extra_high = I2C_SCLH_MAX - sclh_min;

      scll_min += extra_low;
      sclh_min += extra_high;

      t_low_ns  = base_delay_ns +
        (uint64_t)(scll_min + 1U) * t_presc_ns;
      t_high_ns = base_delay_ns +
        (uint64_t)(sclh_min + 1U) * t_presc_ns;
      t_sum_ns  = (uint64_t)t_rise_ns + (uint64_t)t_fall_ns
        + t_high_ns + t_low_ns;
    }

    /* Check basic spec constraints again */
    if (t_low_ns  < t_low_min_ns ||
        t_high_ns < t_high_min_ns)
      continue;

    /* Check I2CCLK constraints:
       tI2CCLK < (tLOW - tfilters)/4 and tI2CCLK < tHIGH
       Approximate tfilters = tAF_max + tDNF
    */
    uint64_t tfilters_ns = tAF_max_ns + tDNF_ns;
    if (!(t_i2cclk_ns < (t_low_ns - tfilters_ns) / 4U &&
          t_i2cclk_ns < t_high_ns)) {
      continue;
    }

    /* Evaluate frequency error */
    uint64_t error = (t_scl_target_ns > t_sum_ns)
      ? (t_scl_target_ns - t_sum_ns)
      : (t_sum_ns - t_scl_target_ns);

    if (!best_valid || error < best_error) {
      best_valid = 1;
      best_error = error;

      best.presc  = (uint8_t)presc;
      best.scldel = (uint8_t)scldel;
      best.sdadel = (uint8_t)sdadel;
      best.sclh   = (uint8_t)sclh_min;
      best.scll   = (uint8_t)scll_min;
    }
  }

  if (!best_valid)
    return -1;

  /* Pack TIMINGR value */
  uint32_t timingr = 0;
  timingr |= ((uint32_t)best.presc  & 0x0FU) << 28;
  timingr |= ((uint32_t)best.scldel & 0x0FU) << 20;
  timingr |= ((uint32_t)best.sdadel & 0x0FU) << 16;
  timingr |= ((uint32_t)best.sclh   & 0xFFU) << 8;
  timingr |= ((uint32_t)best.scll   & 0xFFU);
  return timingr;
}
