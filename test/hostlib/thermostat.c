/*
 * Test firmware for the hostlib prototype: a room thermostat. NOT part of
 * the platform -- it is the "application" a real product build would
 * replace.
 *
 * An ordinary mios app: main() runs on the kernel's main thread and every
 * 100 ms reads a temperature sensor over the (virtual) I2C bus, then drives
 * a heater relay with simple hysteresis. Temperatures are Q8.8 fixed point
 * (1/256 C) so the firmware needs no floating point.
 */
#include <stdio.h>
#include <unistd.h>
#include <mios/io.h>

i2c_t *vi2c_bus(void);   /* provided by the hostlib platform */

#define SENSOR_ADDR   0x48   /* LM75-style temperature sensor            */
#define SENSOR_TEMP   0x00   /* 2 bytes, big-endian, 1/256 C             */
#define RELAY_ADDR    0x20   /* GPIO-expander-style relay output         */
#define RELAY_REG     0x00   /* bit 0 = heater on                        */

#define Q88(c)        ((int)((c) * 256))
#define SETPOINT      Q88(20)     /* 20.0 C */
#define HYSTERESIS    Q88(0.5)    /* +/- 0.5 C */

static int16_t be16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }

int
main(void)
{
  i2c_t *bus = vi2c_bus();
  printf("thermostat: setpoint 20.0 C, +/-0.5 C hysteresis\n");

  int heater = 0;
  while(1) {
    uint8_t t[2];
    if(!i2c_read_bytes(bus, SENSOR_ADDR, SENSOR_TEMP, t, sizeof(t))) {
      int temp = be16(t);
      if(temp < SETPOINT - HYSTERESIS)
        heater = 1;
      else if(temp > SETPOINT + HYSTERESIS)
        heater = 0;

      uint8_t out[2] = { RELAY_REG, (uint8_t)heater };
      i2c_rw(bus, RELAY_ADDR, out, sizeof(out), NULL, 0);
    }
    usleep(100000);   /* 10 Hz control loop (virtual time) */
  }
  return 0;
}
