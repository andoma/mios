#pragma once

typedef struct imu_values {
  float gx, gy, gz; // Gyro
  float ax, ay, az; // Accelerometer
  float mx, my, mz; // Magnetometer
} imu_values_t;
