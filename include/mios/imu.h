#pragma once

typedef struct imu_values {
  float wx, wy, wz; // Angular velocity (rad / s)
  float ax, ay, az; // Acceleration (g)
  float mx, my, mz; // Magnetic flux density (µT)
} imu_values_t;
