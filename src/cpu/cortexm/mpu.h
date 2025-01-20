#pragma once


#define MPU_XN    (1 << 28)
#define MPU_AP_RW (0b011 << 24)

#define MPU_B (1 << 16)
#define MPU_C (1 << 17)
#define MPU_S (1 << 18)

#define MPU_NORMAL_SHARED_NON_CACHED ((0b001 << 19) | MPU_S)

#define MPU_NORMAL_NON_SHARED_NON_CACHED ((0b001 << 19))

void mpu_add_region(void *ptr, int size_power_of_two, uint32_t flags);

void mpu_disable(void);
