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
  virtual buses -- I2C (`vi2c.c`) and SPI (`vspi.c`) register files, and a
  virtual CAN interface (`hostlib_can()`, a real `can_netif` so DSIG/VLLP
  run unmodified; the mios side is `platform/host/vcan.c`) -- the
  write-only console, and the ABI (`libmios.c`/`libmios.h`,
  `hostlib.syms`). An app's board file gets all of it from `hostlib.h`.
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
void     mios_sim_spi_set(uint8_t dev, uint8_t reg, uint8_t val);   /* dev = chip select */
uint8_t  mios_sim_spi_get(uint8_t dev, uint8_t reg);
ssize_t  mios_sim_can_recv(uint32_t *id, void *buf, size_t buflen);  /* -1: none queued */
int      mios_sim_can_send(uint32_t id, const void *payload, size_t len);
```

The register files live in `.data`, so a harness may preset them (chip
ids, ready bits, calibration) before `mios_sim_boot()` and the drivers'
probe sequences in `main()` find a plausible chip. What a register file
does not do is what a live chip does on its own -- re-assert a data-ready
bit after the driver wrote the mode register over it, say -- so a harness
models that by rewriting such registers every step.

## Scope

One instance per `dlmopen()` namespace (glibc allows 16), lockstep virtual
time (real-time pacing is the harness passing wall-clock `dt`). Intended
use: an application built with `PLATFORM=hostlib` from its own `.mk`,
driven by a physical-process simulator -- sensors on the virtual SPI/I2C
register files, actuator commands and telemetry over the virtual CAN.
