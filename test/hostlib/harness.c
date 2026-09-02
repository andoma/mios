/*
 * Prototype simulation harness for mios-as-a-shared-object.
 *
 * A normal glibc program (its own libc) loads the mios thermostat firmware
 * .so into an isolated link-map namespace with dlmopen(), grabs the small
 * sim ABI, and drives it in lockstep virtual time. It plays "the room": a
 * simple thermal model whose temperature the firmware senses over a virtual
 * I2C bus, and whose heater the firmware switches. Two libcs, one process,
 * fully isolated.
 *
 *   cc harness.c -ldl -o harness && ./harness path/to/mios.so
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

#define SENSOR_ADDR 0x48
#define SENSOR_TEMP 0x00
#define RELAY_ADDR  0x20
#define RELAY_REG   0x00

typedef void     (*boot_fn)(void);
typedef void     (*step_fn)(uint64_t);
typedef uint64_t (*time_fn)(void);
typedef void     (*i2c_set_fn)(uint8_t, uint8_t, uint8_t);
typedef uint8_t  (*i2c_get_fn)(uint8_t, uint8_t);

int
main(int argc, char **argv)
{
  const char *so = argc > 1 ? argv[1] : "mios.so";

  void *h = dlmopen(LM_ID_NEWLM, so, RTLD_NOW);   /* own libc namespace */
  if(h == NULL) { fprintf(stderr, "dlmopen: %s\n", dlerror()); return 1; }

  boot_fn    boot = dlsym(h, "mios_sim_boot");
  step_fn    step = dlsym(h, "mios_sim_step");
  time_fn    vtime = dlsym(h, "mios_sim_time");
  i2c_set_fn iset = dlsym(h, "mios_sim_i2c_set");
  i2c_get_fn iget = dlsym(h, "mios_sim_i2c_get");
  if(!boot || !step || !vtime || !iset || !iget) {
    fprintf(stderr, "missing ABI symbol\n"); return 1;
  }

  printf("harness: booting thermostat firmware\n");
  boot();

  double room = 15.0;             /* cold room, degrees C */
  const double ambient = 10.0;    /* it would settle here with no heat */

  for(int s = 0; s < 60; s++) {
    /* publish the current room temperature to the sensor (Q8.8) */
    int16_t raw = (int16_t)(room * 256.0);
    iset(SENSOR_ADDR, SENSOR_TEMP,     (uint8_t)(raw >> 8));
    iset(SENSOR_ADDR, SENSOR_TEMP + 1, (uint8_t)raw);

    step(1000000);   /* advance 1 s; firmware runs ~10 control iterations */

    int heater = iget(RELAY_ADDR, RELAY_REG) & 1;

    /* first-order thermal model over the 1 s step: the heater adds energy,
       the room always leaks toward ambient. Heating wins comfortably, so
       the temperature crosses the setpoint and the thermostat cycles. */
    room += heater ? 1.5 : 0.0;
    room -= 0.08 * (room - ambient);

    printf("harness: t=%2lus  room=%5.2f C  heater=%s\n",
           (unsigned long)(vtime() / 1000000), room, heater ? "ON " : "off");
  }

  dlclose(h);
  return 0;
}
