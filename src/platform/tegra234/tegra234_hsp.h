#pragma once

#include <stdint.h>

#include "reg.h"

// Hardware Synchronization Primitives

#define NV_ADDRESS_MAP_BPMP_HSP_BASE 0x0d150000
#define NV_ADDRESS_MAP_AON_HSP_BASE  0x0c150000
#define NV_ADDRESS_MAP_TOP0_HSP_BASE 0x03c00000
#define NV_ADDRESS_MAP_TOP1_HSP_BASE 0x03d00000

struct stream;

struct stream *
hsp_mbox_stream(uint32_t rx_hsp_base, uint32_t rx_mbox,
                uint32_t tx_hsp_base, uint32_t tx_mbox);

// Low level HSP mbox hookup. Returns address of interrupt enable register
uint32_t hsp_connect_mbox(uint32_t addr, void (*fn)(void *arg), void *arg,
                          uint32_t irq);

#define HSP_MBOX_IRQ_EMPTY(mbox) (0 + (mbox))
#define HSP_MBOX_IRQ_FULL(mbox)  (8 + (mbox))


static inline uint32_t
hsp_mbox_rd(uint32_t base, uint32_t mbox)
{
  return reg_rd(base + 0x10000 + 0x8000 * mbox);
}

static inline void
hsp_mbox_wr(uint32_t base, uint32_t mbox, uint32_t value)
{
  reg_wr(base + 0x10000 + 0x8000 * mbox, 0);
}

static inline uint32_t
hsp_ss_rd(uint32_t base, uint32_t sem)
{
  return reg_rd(base + 0x10000 + 0x8000 * 8 + sem * 0x10000);
}

static inline void
hsp_ss_set(uint32_t base, uint32_t sem, uint32_t bits)
{
  reg_wr(base + 0x10000 + 0x8000 * 8 + sem * 0x10000 + 4, bits);
}

static inline void
hsp_ss_clr(uint32_t base, uint32_t sem, uint32_t bits)
{
  reg_wr(base + 0x10000 + 0x8000 * 8 + sem * 0x10000 + 8, bits);
}

static inline uint32_t
hsp_ss_rd_and_clr(uint32_t base, uint32_t sem)
{
  uint32_t bits = hsp_ss_rd(base, sem);
  hsp_ss_clr(base, sem, bits);
  return bits;
}
