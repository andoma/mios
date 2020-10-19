#include <stdlib.h>

#include "cli.h"



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
hex2nibble(char c)
{
  switch(c) {
  case '0' ... '9':
    return c - '0';
  case 'A' ... 'F':
    return c - 'A' + 10;
  case 'a' ... 'f':
    return c - 'a' + 10;
  default:
    return -1;
  }
}



static uint32_t
atoi_hex(const char *s)
{
  uint32_t r = 0;

  while(1) {
    int v = hex2nibble(*s);
    if(v == -1)
      return r;
    r = r * 16 + v;
    s++;
  }
}

uint32_t
parse_val(const char *s)
{
  while(*s && *s <= 32)
    s++;

  if(s[0] == '0' && s[1] == 'x') {
    return atoi_hex(s + 2);
  }
  return atoi(s);
}


static int
cmd_md(cli_t *cli, int argc, char **argv)
{
  if(argc < 3) {
    cli_printf(cli, "md <start> <length>\n");
    return -1;
  }

  const int start = parse_val(argv[1]);
  const int len   = parse_val(argv[2]);
  mon_hexdump(cli, (void *)start, len, start);
  return 0;
}



CLI_CMD_DEF("md", cmd_md);
