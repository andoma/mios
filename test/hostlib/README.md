# mios as a shared object: hardware-in-the-loop prototype

Proves mios can be built as an isolated Linux shared object and driven by a
normal (glibc) simulation harness, so a physical-process simulator can host
the real device firmware, including its control loop, and exchange
sensor/actuator data over virtual buses.

The worked example is deliberately mundane: a **room thermostat**. The
firmware reads a temperature sensor over I2C and switches a heater relay;
the harness plays the room (a small thermal model). The same shape applies
to any embedded controller.

## What it shows

- `mios.so` is a self-contained shared object: **no `NEEDED` libraries, no
  `DT_INIT_ARRAY`** (mios runs its own constructors), exporting **only** the
  five `mios_sim_*` ABI functions. Its freestanding libc (`malloc`,
  `printf`, ...) is hidden, so it cannot clash with the harness's glibc. The
  harness loads it with `dlmopen(LM_ID_NEWLM)`, giving each instance its own
  link-map namespace.
- The harness drives it in **lockstep virtual time**: `mios_sim_step(dt)`
  runs the firmware forward by `dt` microseconds and returns when it is idle.
  mios and the harness are one OS thread, swapped by a coroutine
  (`cpu_coswitch`); no signals are involved.
- Each step the harness publishes the room temperature to a **virtual I2C
  bus** (`vi2c`); the firmware reads it through the real mios I2C HAL in its
  control loop and switches the heater; the harness reads the heater state
  back and advances its thermal model. Closed loop, in virtual time.

## Layout

- Platform (reusable), `src/platform/hostlib/`: the `.so` build, the library
  boot/step (`src/cpu/host/cpu.c`, `host_lib_boot`/`host_lib_step`), the
  virtual I2C bus (`vi2c.c`), the write-only console, and the ABI
  (`libmios.c`/`libmios.h`, `hostlib.syms`).
- Test (here): `thermostat.c` is stand-in firmware (a real build's
  `main()`), `harness.c` is the glibc room simulator, plus this `Makefile`.

## Run

```
make run
```

builds `build.hostlib/mios.elf` (a shared object) with `thermostat.c` as the
app, builds the harness, and runs it: the room warms from 15 C, crosses the
20 C setpoint, and the thermostat cycles the heater to hold the band.

## ABI (libmios.h)

```
void     mios_sim_boot(void);
void     mios_sim_step(uint64_t dt_us);
uint64_t mios_sim_time(void);
void     mios_sim_i2c_set(uint8_t addr, uint8_t reg, uint8_t val);
uint8_t  mios_sim_i2c_get(uint8_t addr, uint8_t reg);
```

## Prototype scope

Single instance, virtual I2C only (SPI/GPIO/CAN would mirror `vi2c`/`vcan`),
lockstep virtual time (real-time drive would need private signals or a
separate process). Enough to prove the isolation, the coroutine drive, and
the sensor/actuator path end to end.
