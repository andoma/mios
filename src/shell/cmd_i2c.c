#include <stdlib.h>

#include <mios/cli.h>
#include <mios/io.h>

static error_t
cmd_i2c_scan(cli_t *cli, int argc, char **argv)
{
  if(argc != 2) {
    cli_printf(cli, "i2c-scan <bus>\n");
    return ERR_INVALID_ARGS;
  }
  int bus_id = atoi(argv[1]);
  i2c_t *bus = i2c_get_bus(bus_id);
  if(bus == NULL) {
    cli_printf(cli, "i2c bus %d not available\n", bus_id);
    return ERR_NO_DEVICE;
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



static error_t
cmd_i2c_read(cli_t *cli, int argc, char **argv)
{
  if(argc != 5) {
    cli_printf(cli, "i2c-read <bus> <addr> <reg> <length>\n");
    return ERR_INVALID_ARGS;
  }

  const int bus_id = atoi(argv[1]);
  const int addr = atoix(argv[2]);
  const uint8_t reg = atoix(argv[3]);
  const int len = atoix(argv[4]);
  i2c_t *bus = i2c_get_bus(bus_id);
  if(bus == NULL) {
    cli_printf(cli, "i2c bus %d not available\n", bus_id);
    return ERR_NO_DEVICE;
  }

  if(len > 255) {
    cli_printf(cli, "Bad length:%d\n", len);
    return ERR_INVALID_LENGTH;
  }

  uint8_t *buf = malloc(len);

  error_t err = i2c_read_bytes(bus, addr, reg, buf, len);

  if(!err) {
    sthexdump(cli->cl_stream, NULL, buf, len, 0);
  }
  free(buf);
  return err;
}


CLI_CMD_DEF("i2c-read", cmd_i2c_read);


static error_t
cmd_i2c_write(cli_t *cli, int argc, char **argv)
{
  if(argc < 5) {
    cli_printf(cli, "i2c-read <bus> <addr> <reg> <value> ... \n");
    return ERR_INVALID_ARGS;
  }

  const int bus_id = atoi(argv[1]);
  const int addr = atoix(argv[2]);
  const uint8_t reg = atoix(argv[3]);
  i2c_t *bus = i2c_get_bus(bus_id);
  if(bus == NULL) {
    cli_printf(cli, "i2c bus %d not available\n", bus_id);
    return ERR_NO_DEVICE;
  }

  argc -= 4;
  argv += 4;

  uint8_t *buf = malloc(1 + argc);

  buf[0] = reg;
  for(int i = 0; i < argc; i++)
    buf[1 + i] = atoix(argv[i]);

  error_t err = bus->rw(bus, addr, buf, argc + 1, NULL, 0);
  free(buf);
  return err;
}


CLI_CMD_DEF("i2c-write", cmd_i2c_write);
