#include "libmios.h"
#include "vi2c.h"

#include <unistd.h>          /* clock_get */

void host_lib_boot(void);
void host_lib_step(uint64_t dt_us);

#define EXPORT __attribute__((visibility("default")))

EXPORT void     mios_sim_boot(void)            { host_lib_boot(); }
EXPORT void     mios_sim_step(uint64_t dt)     { host_lib_step(dt); }
EXPORT uint64_t mios_sim_time(void)            { return clock_get(); }

EXPORT void
mios_sim_i2c_set(uint8_t addr, uint8_t reg, uint8_t val)
{
  vi2c_set_reg(addr, reg, val);
}

EXPORT uint8_t
mios_sim_i2c_get(uint8_t addr, uint8_t reg)
{
  return vi2c_get_reg(addr, reg);
}
