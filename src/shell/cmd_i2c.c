#include <stdlib.h>

#include <mios/cli.h>
#include <mios/io.h>

#include "monitor.h"

static int
cmd_i2c_scan(cli_t *cli, int argc, char **argv)
{
  if(argc != 2) {
    cli_printf(cli, "i2c-scan <bus>\n");
    return -1;
  }
  int bus_id = atoi(argv[1]);
  i2c_t *bus = i2c_get_bus(bus_id);
  if(bus == NULL) {
    cli_printf(cli, "i2c bus %d not available\n", bus_id);
    return 0;
  }

  for(int i = 0; i < 128; i++) {
    uint8_t u8;
    cli_printf(cli, "%02x: ", i);
    error_t err = i2c_read_u8(bus, i, 0, &u8);
    if(err) {
      cli_printf(cli, "ERR: %d\n", err);
    } else {
      cli_printf(cli, "OK : 0x%x\n", u8);
    }
  }
  return 0;
}


CLI_CMD_DEF("i2c-scan", cmd_i2c_scan);



static int
cmd_i2c_read(cli_t *cli, int argc, char **argv)
{
  if(argc != 5) {
    cli_printf(cli, "i2c-read <bus> <addr> <reg> <length>\n");
    return -1;
  }

  const int bus_id = atoi(argv[1]);
  const int addr = atoi(argv[2]);
  const int reg = atoi(argv[3]);
  const int len = atoi(argv[4]);
  i2c_t *bus = i2c_get_bus(bus_id);
  if(bus == NULL) {
    cli_printf(cli, "i2c bus %d not available\n", bus_id);
    return 0;
  }

  if(len > 256) {
    cli_printf(cli, "Bad length:%d\n", len);
    return 0;
  }

  uint8_t *buf = malloc(len);

  error_t err = i2c_read_bytes(bus, addr, reg, buf, len);

  if(err) {
    cli_printf(cli, "ERR: %d\n", err);
  } else {
    mon_hexdump(cli, buf, len, 0);
  }
  free(buf);
  return 0;
}


CLI_CMD_DEF("i2c-read", cmd_i2c_read);
