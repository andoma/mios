#pragma once

#include <mios/io.h>

// ST TCPP03 USB Type-C port protection and power path controller

typedef struct tcpp03 tcpp03_t;

// address is the 7-bit I2C address (0x34 or 0x35). enable is the EN pin,
// GPIO_UNUSED if it is tied high. The device starts out in low power
// mode with the sink path closed, suitable for a toggling dual role port
tcpp03_t *tcpp03_create(i2c_t *i2c, uint8_t address, gpio_t enable,
                        const char *name);

// Optional VBUS measurement (in mV), used to wait for VBUS to discharge
void tcpp03_set_vbus_monitor(tcpp03_t *t, int (*vbus_mv)(void *opaque),
                             void *opaque);

// Monitor the FLGn output (open drain, active low). In normal mode it
// signals faults, which are logged and counted
void tcpp03_set_flag_gpio(tcpp03_t *t, gpio_t flg);

// Attached as source: open the sink path and turn on VBUS
error_t tcpp03_source_on(tcpp03_t *t);

// Attached as sink: enable the protections, the sink path stays closed
error_t tcpp03_sink_on(tcpp03_t *t);

// Detached: turn off and discharge VBUS if we were the source, then go
// back to low power mode with the sink path closed
error_t tcpp03_detach(tcpp03_t *t);
