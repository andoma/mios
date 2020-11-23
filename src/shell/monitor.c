#include <stdlib.h>

#include <mios/cli.h>



static void
mon_hexdump(cli_t *cli, const void *data_, int len, int offset)
{
  int i, j, k;
  const uint8_t *data = data_;

  for(i = 0; i < len; i+= 16) {
    cli_printf(cli, "0x%08x: ", i + offset);

    for(j = 0; j + i < len && j < 16; j++) {
      cli_printf(cli, "%s%02x ", j==8 ? " " : "", data[i+j]);
    }
    const int cnt = (17 - j) * 3 + (j < 8);
    for(k = 0; k < cnt; k++)
      cli_printf(cli, " ");

    for(j = 0; j + i < len && j < 16; j++) {
      char c = data[i+j] < 32 || data[i+j] > 126 ? '.' : data[i+j];
      cli_printf(cli, "%c", c);
    }
    cli_printf(cli, "\n");
  }
}


static int
cmd_md(cli_t *cli, int argc, char **argv)
{
  if(argc < 3) {
    cli_printf(cli, "md <start> <length>\n");
    return -1;
  }

  const int start = atoix(argv[1]);
  const int len   = atoix(argv[2]);
  mon_hexdump(cli, (void *)start, len, start);
  return 0;
}



CLI_CMD_DEF("md", cmd_md);
