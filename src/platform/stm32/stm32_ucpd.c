// STM32 UCPD (USB Type-C / Power Delivery interface) as a Type-C PHY.
//
// Same IP on STM32G4 and STM32N6 (the G4 also has fast role swap
// detection). Included from platform code, which enables the clock,
// configures pins and handles dead battery release where applicable.
//
// Only Type-C detection so far, USB PD messaging is not implemented.

#include <stdlib.h>

#include <usb/typec.h>

#include "irq.h"

#define UCPD_CFGR1  0x00
#define UCPD_CFGR2  0x04
#define UCPD_CR     0x0c
#define UCPD_IMR    0x10
#define UCPD_SR     0x14
#define UCPD_ICR    0x18

#define UCPD_CR_ANAMODE     (1 << 9)
#define UCPD_CR_PHYCCSEL    (1 << 6)

#define UCPD_TYPECEVT1      (1 << 14)
#define UCPD_TYPECEVT2      (1 << 15)

typedef struct stm32_ucpd {
  typec_phy_t su_phy;
  uint32_t su_base;
  uint8_t su_sink;  // ANAMODE is sink, decides how VSTATE is decoded
} stm32_ucpd_t;


static void
ucpd_set_pull(typec_phy_t *tp, typec_pull_t pull)
{
  stm32_ucpd_t *su = (stm32_ucpd_t *)tp;

  uint32_t cr = reg_rd(su->su_base + UCPD_CR);
  cr &= ~((3 << 10) | UCPD_CR_ANAMODE | (3 << 7));
  cr |= 3 << 10; // CCENABLE: Both CC lines

  if(pull == TYPEC_PULL_RD) {
    cr |= UCPD_CR_ANAMODE;
  } else {
    // ANASUBMODE: 1 = default USB, 2 = 1.5A, 3 = 3.0A
    cr |= (pull - TYPEC_PULL_RP_DEF + 1) << 7;
  }
  su->su_sink = pull == TYPEC_PULL_RD;
  reg_wr(su->su_base + UCPD_CR, cr);
}


static typec_cc_t
decode_vstate(int sink, int v)
{
  if(sink) {
    static const typec_cc_t sink_states[4] = {
      TYPEC_CC_OPEN, TYPEC_CC_RP_DEF, TYPEC_CC_RP_1A5, TYPEC_CC_RP_3A0
    };
    return sink_states[v];
  }
  // RM0486 table 755 has these one column off, this is what ST's own
  // Type-C code uses (00 = vRa, 01 = vRd, 10 = vOPEN)
  static const typec_cc_t source_states[4] = {
    TYPEC_CC_RA, TYPEC_CC_RD, TYPEC_CC_OPEN, TYPEC_CC_OPEN
  };
  return source_states[v];
}


static void
ucpd_get_cc(typec_phy_t *tp, typec_cc_t cc[2])
{
  stm32_ucpd_t *su = (stm32_ucpd_t *)tp;
  const uint32_t sr = reg_rd(su->su_base + UCPD_SR);
  cc[0] = decode_vstate(su->su_sink, (sr >> 16) & 3);
  cc[1] = decode_vstate(su->su_sink, (sr >> 18) & 3);
}


static void
ucpd_set_polarity(typec_phy_t *tp, int cc)
{
  stm32_ucpd_t *su = (stm32_ucpd_t *)tp;
  if(cc)
    reg_set_bit(su->su_base + UCPD_CR, 6);
  else
    reg_clr_bit(su->su_base + UCPD_CR, 6);
}


static const typec_phy_vtable_t ucpd_vtable = {
  .set_pull = ucpd_set_pull,
  .get_cc = ucpd_get_cc,
  .set_polarity = ucpd_set_polarity,
};


static void
ucpd_irq(void *arg)
{
  stm32_ucpd_t *su = arg;
  const uint32_t sr = reg_rd(su->su_base + UCPD_SR);
  const uint32_t evt = sr & (UCPD_TYPECEVT1 | UCPD_TYPECEVT2);

  if(evt) {
    reg_wr(su->su_base + UCPD_ICR, evt);
    if(su->su_phy.tp_cc_changed != NULL)
      su->su_phy.tp_cc_changed(su->su_phy.tp_opaque);
  }
}


// kernel_clk is the frequency of ucpd_ker_ck
static typec_phy_t *
stm32_ucpd_create(uint32_t base, int irq, unsigned int kernel_clk)
{
  stm32_ucpd_t *su = calloc(1, sizeof(stm32_ucpd_t));
  su->su_phy.tp_vtable = &ucpd_vtable;
  su->su_base = base;

  // ucpd_clk should be 6-12 MHz and the half bit clock about 600 kHz
  int psc = 0;
  while((kernel_clk >> psc) > 12000000)
    psc++;
  const unsigned int ucpd_clk = kernel_clk >> psc;
  const unsigned int hbitclkdiv = (ucpd_clk + 300000) / 600000;

  reg_wr(base + UCPD_CFGR1,
         (psc << 17) |             // PSC_UCPDCLK
         (7 << 11) |               // TRANSWIN
         (16 << 6) |               // IFRGAP
         (hbitclkdiv - 1));        // HBITCLKDIV
  reg_set_bit(base + UCPD_CFGR1, 31); // UCPDEN

  reg_wr(base + UCPD_ICR, UCPD_TYPECEVT1 | UCPD_TYPECEVT2);
  reg_wr(base + UCPD_IMR, UCPD_TYPECEVT1 | UCPD_TYPECEVT2);
  irq_enable_fn_arg(irq, IRQ_LEVEL_IO, ucpd_irq, su);
  return &su->su_phy;
}
