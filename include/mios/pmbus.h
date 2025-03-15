#pragma once

#include <mios/error.h>
#include <stdint.h>

struct i2c;
typedef struct pmbus pmbus_t;

// Low level helpers for PMBUS
// Can be expanded into high level API in the future

pmbus_t *pmbus_create(struct i2c *bus, uint8_t address, int com_interval_us);

// Returns negative number if error
int pmbus_read_8(pmbus_t *pmbus, uint8_t reg);

// Returns negative number if error
int pmbus_read_16(pmbus_t *pmbus, uint8_t reg);

error_t pmbus_write_8(pmbus_t *pmbus, uint8_t reg, uint8_t value);

float pmbus_decode_linear11(int16_t value);
