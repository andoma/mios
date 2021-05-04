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
  cli_printf(cl, NULL);
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
  cli_printf(cl, NULL);
}



static size_t
cli_stream_fmt(void *arg, const char *buf, size_t len)
{
  stream_t *s = arg;
  s->write(s, buf, len);
  return len;
}


static void
cli_stream_printf(struct cli *cli, const char *fmt, ...)
{
  stream_t *s = cli->cl_opaque;

  if(fmt == NULL) {
    s->write(s, NULL, 0);
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  fmtv(cli_stream_fmt, s, fmt, ap);
  va_end(ap);
}


static int
cli_stream_getc(struct cli *cli, int wait)
{
  stream_t *s = cli->cl_opaque;
  if(s->read == NULL)
    return ERR_NOT_IMPLEMENTED;

  char c;
  int r = s->read(s, &c, 1,
                  wait ? STREAM_READ_WAIT_ONE : STREAM_READ_WAIT_NONE);
  if(r == 0)
    return ERR_NOT_READY;
  return c;
}


void
cli_on_stream(stream_t *s)
{
  cli_t cli = {
    .cl_printf = cli_stream_printf,
    .cl_getc = cli_stream_getc,
    .cl_opaque = s
  };
  cli_printf(&cli, "\n");
  cli_prompt(&cli);
  while(1) {
    int c = cli_getc(&cli, 1);
    if(c < 0)
      break;
    cli_input_char(&cli, c);
  }
}


void
cli_console(void)
{
  cli_on_stream(stdio);
}
