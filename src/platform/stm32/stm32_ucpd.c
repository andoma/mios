// STM32 UCPD (USB Type-C / Power Delivery interface) as a Type-C PHY.
//
// Same IP on STM32G4 and STM32N6 (the G4 also has fast role swap
// detection). Included from platform code, which enables the clock,
// configures pins and handles dead battery release where applicable.
//
// USB PD packets are moved byte by byte from the interrupt handler.
// At 300 kbit/s a byte takes about 33 us.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mios/error.h>
#include <mios/stream.h>

#include <usb/typec.h>

#include "irq.h"

#define UCPD_CFGR1  0x00
#define UCPD_CFGR2  0x04
#define UCPD_CR     0x0c
#define UCPD_IMR    0x10
#define UCPD_SR     0x14
#define UCPD_ICR    0x18
#define UCPD_TX_ORDSETR 0x1c
#define UCPD_TX_PAYSZR  0x20
#define UCPD_TXDR       0x24
#define UCPD_RX_ORDSETR 0x28
#define UCPD_RX_PAYSZR  0x2c
#define UCPD_RXDR       0x30

#define UCPD_CR_ANAMODE     (1 << 9)
#define UCPD_CR_PHYCCSEL    (1 << 6)
#define UCPD_CR_PHYRXEN     (1 << 5)
#define UCPD_CR_TXHRST      (1 << 3)
#define UCPD_CR_TXSEND      (1 << 2)

// SR, IMR and ICR share bit positions
#define UCPD_TXIS           (1 << 0)
#define UCPD_TXMSGDISC      (1 << 1)
#define UCPD_TXMSGSENT      (1 << 2)
#define UCPD_TXMSGABT       (1 << 3)
#define UCPD_HRSTDISC       (1 << 4)
#define UCPD_HRSTSENT       (1 << 5)
#define UCPD_TXUND          (1 << 6)
#define UCPD_RXNE           (1 << 8)
#define UCPD_RXORDDET       (1 << 9)
#define UCPD_RXHRSTDET      (1 << 10)
#define UCPD_RXOVR          (1 << 11)
#define UCPD_RXMSGEND       (1 << 12)
#define UCPD_RXERR          (1 << 13)
#define UCPD_TYPECEVT1      (1 << 14)
#define UCPD_TYPECEVT2      (1 << 15)

#define UCPD_PD_IRQS (UCPD_TXIS | UCPD_TXMSGDISC | UCPD_TXMSGSENT |        \
                      UCPD_TXMSGABT | UCPD_HRSTDISC | UCPD_HRSTSENT |      \
                      UCPD_TXUND | UCPD_RXNE | UCPD_RXORDDET |             \
                      UCPD_RXHRSTDET | UCPD_RXOVR | UCPD_RXMSGEND)

// Four K-codes (Sync-1 x 3, Sync-2), first one in the low bits
#define K_SYNC1 0x18
#define K_SYNC2 0x11
#define ORDSET_SOP (K_SYNC1 | (K_SYNC1 << 5) | (K_SYNC1 << 10) | (K_SYNC2 << 15))

#define PD_MAX_PACKET 30  // Header and 7 data objects, no extended messages

typedef struct stm32_ucpd {
  typec_phy_t su_phy;
  uint32_t su_base;
  uint8_t su_sink;  // ANAMODE is sink, decides how VSTATE is decoded

  uint8_t su_rx_sop;  // Packet being received is SOP
  uint8_t su_rx_len;
  uint8_t su_tx_len;
  uint8_t su_tx_pos;
  uint8_t su_tx_busy;
  uint8_t su_txq_len;  // A packet waiting for the current one to finish
  uint8_t su_rx_buf[PD_MAX_PACKET];
  uint8_t su_tx_buf[PD_MAX_PACKET];
  uint8_t su_txq_buf[PD_MAX_PACKET];

  uint32_t su_rx_packets;
  uint32_t su_rx_errors;
  uint32_t su_rx_overruns;
  uint32_t su_tx_packets;
  uint32_t su_tx_discarded;
  uint32_t su_tx_underruns;
  uint32_t su_hard_resets_rx;
} stm32_ucpd_t;


// CR is modified from threads and from the interrupt handler
static void
ucpd_cr_modify(stm32_ucpd_t *su, uint32_t clr, uint32_t set)
{
  int q = irq_forbid(IRQ_LEVEL_IO);
  const uint32_t cr = reg_rd(su->su_base + UCPD_CR);
  reg_wr(su->su_base + UCPD_CR, (cr & ~clr) | set);
  irq_permit(q);
}


static void
ucpd_set_pull(typec_phy_t *tp, typec_pull_t pull)
{
  stm32_ucpd_t *su = (stm32_ucpd_t *)tp;

  uint32_t set = 3 << 10; // CCENABLE: Both CC lines
  if(pull == TYPEC_PULL_RD) {
    set |= UCPD_CR_ANAMODE;
  } else {
    // ANASUBMODE: 1 = default USB, 2 = 1.5A, 3 = 3.0A
    set |= (pull - TYPEC_PULL_RP_DEF + 1) << 7;
  }
  su->su_sink = pull == TYPEC_PULL_RD;
  ucpd_cr_modify(su, (3 << 10) | UCPD_CR_ANAMODE | (3 << 7), set);
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
  ucpd_cr_modify(su, UCPD_CR_PHYCCSEL, cc ? UCPD_CR_PHYCCSEL : 0);
}


static void
ucpd_pd_rx_enable(typec_phy_t *tp, int on)
{
  stm32_ucpd_t *su = (stm32_ucpd_t *)tp;
  ucpd_cr_modify(su, UCPD_CR_PHYRXEN, on ? UCPD_CR_PHYRXEN : 0);
}


// Called with IRQ_LEVEL_IO blocked
static void
ucpd_tx_start(stm32_ucpd_t *su, const uint8_t *buf, size_t len)
{
  memcpy(su->su_tx_buf, buf, len);
  su->su_tx_len = len;
  su->su_tx_pos = 0;
  su->su_tx_busy = 1;
  reg_wr(su->su_base + UCPD_TX_ORDSETR, ORDSET_SOP);
  reg_wr(su->su_base + UCPD_TX_PAYSZR, len);
  ucpd_cr_modify(su, 3, UCPD_CR_TXSEND); // TXMODE 0: Normal packet
}


static void
ucpd_pd_transmit(typec_phy_t *tp, const uint8_t *buf, size_t len)
{
  stm32_ucpd_t *su = (stm32_ucpd_t *)tp;
  if(len > PD_MAX_PACKET)
    len = PD_MAX_PACKET;

  int q = irq_forbid(IRQ_LEVEL_IO);
  if(su->su_tx_busy) {
    // Typically a reply right after our GoodCRC
    memcpy(su->su_txq_buf, buf, len);
    su->su_txq_len = len;
  } else {
    ucpd_tx_start(su, buf, len);
  }
  irq_permit(q);
}


static void
ucpd_pd_hard_reset(typec_phy_t *tp)
{
  stm32_ucpd_t *su = (stm32_ucpd_t *)tp;
  int q = irq_forbid(IRQ_LEVEL_IO);
  su->su_txq_len = 0;
  ucpd_cr_modify(su, 0, UCPD_CR_TXHRST);
  irq_permit(q);
}


static void
ucpd_print_info(typec_phy_t *tp, struct stream *st)
{
  stm32_ucpd_t *su = (stm32_ucpd_t *)tp;
  stprintf(st, "PD RX: %d packets, %d CRC errors, %d overruns, "
           "%d hard resets\n",
           su->su_rx_packets, su->su_rx_errors, su->su_rx_overruns,
           su->su_hard_resets_rx);
  stprintf(st, "PD TX: %d packets, %d discarded, %d underruns\n",
           su->su_tx_packets, su->su_tx_discarded, su->su_tx_underruns);
}


static const typec_phy_vtable_t ucpd_vtable = {
  .print_info = ucpd_print_info,
  .set_pull = ucpd_set_pull,
  .get_cc = ucpd_get_cc,
  .set_polarity = ucpd_set_polarity,
  .pd_rx_enable = ucpd_pd_rx_enable,
  .pd_transmit = ucpd_pd_transmit,
  .pd_hard_reset = ucpd_pd_hard_reset,
};


static void
ucpd_irq(void *arg)
{
  stm32_ucpd_t *su = arg;
  typec_phy_t *tp = &su->su_phy;
  const uint32_t base = su->su_base;
  const uint32_t sr = reg_rd(base + UCPD_SR) & reg_rd(base + UCPD_IMR);

  // Receive. Bytes before end of message, as both may be pending
  if(sr & UCPD_RXORDDET) {
    reg_wr(base + UCPD_ICR, UCPD_RXORDDET);
    su->su_rx_sop = (reg_rd(base + UCPD_RX_ORDSETR) & 7) == 0;
    su->su_rx_len = 0;
  }

  if(sr & UCPD_RXNE) {
    const uint8_t b = reg_rd(base + UCPD_RXDR);
    if(su->su_rx_len < PD_MAX_PACKET)
      su->su_rx_buf[su->su_rx_len++] = b;
  }

  if(sr & UCPD_RXOVR) {
    reg_wr(base + UCPD_ICR, UCPD_RXOVR);
    su->su_rx_overruns++;
  }

  if(sr & UCPD_RXMSGEND) {
    const int err = reg_rd(base + UCPD_SR) & UCPD_RXERR;
    reg_wr(base + UCPD_ICR, UCPD_RXMSGEND);
    if(err) {
      su->su_rx_errors++;
    } else if(su->su_rx_sop) {
      su->su_rx_packets++;
      if(tp->tp_pd_rx != NULL)
        tp->tp_pd_rx(tp->tp_pd_opaque, su->su_rx_buf, su->su_rx_len);
    }
  }

  if(sr & UCPD_RXHRSTDET) {
    reg_wr(base + UCPD_ICR, UCPD_RXHRSTDET);
    su->su_hard_resets_rx++;
    if(tp->tp_pd_hard_reset != NULL)
      tp->tp_pd_hard_reset(tp->tp_pd_opaque, 1);
  }

  // Transmit
  if(sr & UCPD_TXIS) {
    if(su->su_tx_pos < su->su_tx_len)
      reg_wr(base + UCPD_TXDR, su->su_tx_buf[su->su_tx_pos++]);
  }

  if(sr & UCPD_TXUND) {
    reg_wr(base + UCPD_ICR, UCPD_TXUND);
    su->su_tx_underruns++;
  }

  if(sr & (UCPD_TXMSGSENT | UCPD_TXMSGDISC | UCPD_TXMSGABT)) {
    reg_wr(base + UCPD_ICR, UCPD_TXMSGSENT | UCPD_TXMSGDISC | UCPD_TXMSGABT);
    int status = 0;
    if(sr & UCPD_TXMSGSENT) {
      su->su_tx_packets++;
    } else {
      su->su_tx_discarded++;
      status = ERR_TX;
    }
    su->su_tx_busy = 0;
    if(tp->tp_pd_tx_done != NULL)
      tp->tp_pd_tx_done(tp->tp_pd_opaque, status);
    if(!su->su_tx_busy && su->su_txq_len) {
      ucpd_tx_start(su, su->su_txq_buf, su->su_txq_len);
      su->su_txq_len = 0;
    }
  }

  if(sr & (UCPD_HRSTSENT | UCPD_HRSTDISC)) {
    reg_wr(base + UCPD_ICR, UCPD_HRSTSENT | UCPD_HRSTDISC);
    su->su_tx_busy = 0;
    if(tp->tp_pd_hard_reset != NULL)
      tp->tp_pd_hard_reset(tp->tp_pd_opaque, 0);
  }

  // Type-C
  const uint32_t evt = sr & (UCPD_TYPECEVT1 | UCPD_TYPECEVT2);
  if(evt) {
    reg_wr(base + UCPD_ICR, evt);
    if(tp->tp_cc_changed != NULL)
      tp->tp_cc_changed(tp->tp_opaque);
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
         (0x9 << 20) |             // RXORDSETEN: SOP and Hard Reset
         (psc << 17) |             // PSC_UCPDCLK
         (7 << 11) |               // TRANSWIN
         (16 << 6) |               // IFRGAP
         (hbitclkdiv - 1));        // HBITCLKDIV
  reg_set_bit(base + UCPD_CFGR1, 31); // UCPDEN

  reg_wr(base + UCPD_ICR, 0xffff);
  reg_wr(base + UCPD_IMR, UCPD_TYPECEVT1 | UCPD_TYPECEVT2 | UCPD_PD_IRQS);
  irq_enable_fn_arg(irq, IRQ_LEVEL_IO, ucpd_irq, su);
  return &su->su_phy;
}
