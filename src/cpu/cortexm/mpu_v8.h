#pragma once

#include <stdint.h>

// PMSAv8-M RBAR flags (bits [4:0] of RBAR)
#define MPU_V8_XN       (1 << 0)  // Execute Never
#define MPU_V8_AP_RW_RW (1 << 1)  // Full access (priv + unpriv)
#define MPU_V8_AP_RW    (0 << 1)  // Priv RW, unpriv no access
#define MPU_V8_AP_RO_RO (3 << 1)  // Read-only (priv + unpriv)
#define MPU_V8_AP_RO    (2 << 1)  // Priv RO, unpriv no access
#define MPU_V8_SH_NONE  (0 << 3)  // Non-shareable
#define MPU_V8_SH_OUTER (2 << 3)  // Outer shareable
#define MPU_V8_SH_INNER (3 << 3)  // Inner shareable

// PMSAv8-M RLAR AttrIndx (bits [3:1] of RLAR) - index into MAIR
#define MPU_V8_ATTR_DEVICE        (0 << 1)  // MAIR Attr0: Device-nGnRnE
#define MPU_V8_ATTR_NORMAL_NC     (1 << 1)  // MAIR Attr1: Normal Non-cacheable
#define MPU_V8_ATTR_NORMAL_WT     (2 << 1)  // MAIR Attr2: Normal Write-Through
#define MPU_V8_ATTR_NORMAL_WB     (3 << 1)  // MAIR Attr3: Normal Write-Back

void mpu_disable(void);
void mpu_enable(void);

void mpu_setup_region(int region, uint32_t base, uint32_t limit,
                      uint32_t rbar_flags, uint32_t rlar_flags);

int mpu_add_region_v8(uint32_t base, uint32_t limit,
                      uint32_t rbar_flags, uint32_t rlar_flags);

void mpu_protect_code(int on);
