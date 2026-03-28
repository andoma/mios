#pragma once

#include "stm32n6_reg.h"

// RIFSC base address (secure alias)
#define RIFSC_BASE 0x44024000

// RISAF base addresses (secure alias)
#define RISAF2_BASE 0x44027000  // AXISRAM1
#define RISAF3_BASE 0x44028000  // AXISRAM2

// RIFSC RIMC master indices
#define RIF_MASTER_ETH1  6

// Configure RIMC master attributes (CID, secure, privileged)
static inline void
stm32n6_rif_set_master_attr(int master_idx, int cid, int sec, int priv)
{
  reg_wr(RIFSC_BASE + 0xC10 + master_idx * 4,
         (cid << 4) | (sec << 8) | (priv << 9));
}

// Configure RISC peripheral security attributes
// periph_reg: register index (0-5), bit: bit position within register
static inline void
stm32n6_rif_set_periph_sec(int periph_reg, int bit, int sec, int priv)
{
  if(sec)
    reg_set_bit(RIFSC_BASE + 0x10 + periph_reg * 4, bit);
  if(priv)
    reg_set_bit(RIFSC_BASE + 0x30 + periph_reg * 4, bit);
}

// ETH1 is in RISC register 1, bit 28
#define RIF_PERIPH_ETH1_REG  1
#define RIF_PERIPH_ETH1_BIT  28

// Configure a RISAF base region to allow specified CID mask read+write access
// risaf_base: RISAF instance base address
// cid_mask: bitmask of CIDs to allow (e.g. 0xFF for all)
static inline void
stm32n6_risaf_open_base_region(uint32_t risaf_base, uint32_t cid_mask)
{
  // Clear illegal access flags
  reg_wr(risaf_base + 0x0C, 0x3);
  // REG0_CIDCFGR: RDENC = cid_mask, WRENC = cid_mask
  reg_wr(risaf_base + 0x4C, cid_mask | (cid_mask << 16));
  // REG0_CFGR: BREN=1, PRIVC for all CIDs
  reg_wr(risaf_base + 0x40, 0x00FF0001);
}
