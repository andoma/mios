#include <mios/cli.h>
#include <mios/sys.h>

const struct serial_number
sys_get_serial_number(void)
{
  struct serial_number sn;
  sn.data = (const void *)0x1fff7a10;
  sn.len = 12;
  return sn;
}
