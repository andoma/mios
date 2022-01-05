#include <string.h>
#include <stdio.h>

#include <mios/cli.h>
#include <mios/error.h>
#include <mios/version.h>

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



static void
cli_prompt(cli_t *cl, char promptchar)
{
  cli_printf(cl, "%c ", promptchar);
  cli_printf(cl, NULL);
}

void
cli_input_char(cli_t *cl, char c, char promptchar)
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
    cli_prompt(cl, promptchar);
    break;
  default:
    //    printf("\n\nGot code %d\n", c);
    break;
  }
  cli_printf(cl, NULL);
}


int
cli_getc(struct cli *cli, int wait)
{
  stream_t *s = cli->cl_stream;
  if(s->read == NULL)
    return ERR_NOT_IMPLEMENTED;

  char c;
  int r = s->read(s, &c, 1,
                  wait ? STREAM_READ_WAIT_ONE : STREAM_READ_WAIT_NONE);
  if(r == 0)
    return ERR_NOT_READY;
  if(r < 0)
    return -1;
  return c;
}

int
cli_on_stream(stream_t *s, char promptchar)
{
  cli_t cli = {
    .cl_stream = s
  };
  stprintf(s, "\n");
  mios_print_version(s);
  cli_prompt(&cli, promptchar);
  while(1) {
    int c = cli_getc(&cli, 1);
    if(c < 0)
      return -1;
    if(c == 4)
      return 0;
    cli_input_char(&cli, c, promptchar);
  }
}


void
cli_console(char promptchar)
{
  if(stdio == NULL)
    return;
  while(1) {
    if(cli_on_stream(stdio, promptchar) < 0)
      return;
  }
}
