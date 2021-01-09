#include <string.h>
#include <stdio.h>

#include <mios/cli.h>
#include <mios/error.h>

static int
tokenize(char *buf, char **vec, int vecsize)
{
  int n = 0;

  while(1) {
    while((*buf > 0 && *buf < 33))
      buf++;
    if(*buf == 0)
      break;
    vec[n++] = buf;
    if(n == vecsize)
      break;
    while(*buf > 32)
      buf++;
    if(*buf == 0)
      break;
    *buf = 0;
    buf++;
  }
  return n;
}

#define CLI_MAX_ARGC 8

int
dispatch_command(cli_t *c, char *line)
{
  char *argv[CLI_MAX_ARGC];
  int argc = tokenize(line, argv, CLI_MAX_ARGC);
  if(argc == 0)
    return 0;

  extern unsigned long _clicmd_array_begin;
  extern unsigned long _clicmd_array_end;

  cli_cmd_t *clicmd = (void *)&_clicmd_array_begin;
  cli_cmd_t *clicmd_array_end = (void *)&_clicmd_array_end;

  if(!strcmp(argv[0], "help")) {
    cli_printf(c, "Available commands:\n");
    for(; clicmd != clicmd_array_end; clicmd++) {
      cli_printf(c, "\t%s\n", clicmd->cmd);
    }
    return 0;
  }

  for(; clicmd != clicmd_array_end; clicmd++) {
    if(!strcmp(argv[0], clicmd->cmd)) {
      return clicmd->dispatch(c, argc, argv);
    }
  }
  cli_printf(c, "No such command\n");
  return 1;
}



void
cli_prompt(cli_t *cl)
{
  cli_printf(cl, "> ");
}

void
cli_input_char(cli_t *cl, char c)
{
  switch((uint8_t)c) {
  case 127:
  case 8:
    // Backspace
    if(cl->cl_pos) {
      cl->cl_pos--;
      cl->cl_buf[cl->cl_pos] = 0;
      cli_printf(cl, "\033[D\033[K");
    }
    break;

  case 32 ... 126:
    if(cl->cl_pos < CLI_LINE_BUF_SIZE - 1) {
      cl->cl_buf[cl->cl_pos++] = c;
      cl->cl_buf[cl->cl_pos] = 0;
      cli_printf(cl, "%c", c);
    }
    break;
  case 10:
  case 13:
    cli_printf(cl, "\r\n");
    dispatch_command(cl, cl->cl_buf);
    cl->cl_pos = 0;
    cl->cl_buf[cl->cl_pos] = 0;
    cli_prompt(cl);
    break;
  default:
    //    printf("\n\nGot code %d\n", c);
    break;
  }
}

static void
cli_console_printf(struct cli *cli, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}


static int
cli_console_getc(struct cli *cli, int wait)
{
  char c;

  if(stdio == NULL)
    return ERR_NOT_IMPLEMENTED;

  int r = stdio->read(stdio, &c, 1,
                      wait ? STREAM_READ_WAIT_ONE : STREAM_READ_WAIT_NONE);
  if(r == 0)
    return ERR_NOT_READY;
  return c;
}



void
cli_console(void)
{
  cli_t cli = {};
  cli.cl_printf = cli_console_printf;
  cli.cl_getc = cli_console_getc;

  cli_prompt(&cli);
  while(1) {
    int c = getchar();
    if(c < 0)
      break;
    cli_input_char(&cli, c);
  }
}
