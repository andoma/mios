#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/cli.h>
#include <mios/mios.h>
#include <mios/error.h>
#include <mios/version.h>
#include <mios/hostname.h>

#include "history.h"

static const char errmsg[] = {
  "OK\0"
  "NOT_IMPLEMENTED\0"
  "TIMEOUT\0"
  "OPERATION_FAILED\0"
  "TX\0"
  "RX\0"
  "NOT_READY\0"
  "NO_BUFFER\0"
  "MTU_EXCEEDED\0"
  "INVALID_ID\0"
  "DMAXFER\0"
  "BUS_ERR\0"
  "ARBITRATION_LOST\0"
  "BAD_STATE\0"
  "INVALID_ADDRESS\0"
  "NO_DEVICE\0"
  "MISMATCH\0"
  "NOT_FOUND\0"
  "CHECKSUM_ERR\0"
  "MALFORMED\0"
  "INVALID_RPC_ID\0"
  "INVALID_RPC_ARGS\0"
  "NO_FLASH_SPACE\0"
  "INVALID_ARGS\0"
  "INVALID_LENGTH\0"
  "NOT_IDLE\0"
  "BAD_CONFIG\0"
  "FLASH_HW_ERR\0"
  "FLASH_TIMEOUT\0"
  "NO_MEMORY\0"
  "READ_PROT\0"
  "WRITE_PROT\0"
  "AGAIN\0"
  "NOT_CONNECTED\0"
  "BAD_PKT_SIZ\0"
  "EXISTS\0"
  "CORRUPT\0"
  "NOT_DIR\0"
  "IS_DIR\0"
  "NOT_EMPTY\0"
  "BADF\0"
  "TOOBIG\0"
  "INVALID_PARAMETER\0"
  "NOTATTR\0"
  "TOOLONG\0"
  "IO\0"
  "FS\0"
  "DMAFIFO\0"
  "INTERRUPTED\0"
  "QUEUE_FULL\0"
  "NO_ROUTE\0"
  "\0"
};


const char *
error_to_string(error_t e)
{
  return strtbl(errmsg, -e);
}

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


static void
dispatch_command(cli_t *c, char *line)
{
  int argc = tokenize(line, c->cl_argv, CLI_MAX_ARGC);
  if(argc == 0)
    return;

  extern unsigned long _clicmd_array_begin;
  extern unsigned long _clicmd_array_end;

  cli_cmd_t *clicmd = (void *)&_clicmd_array_begin;
  cli_cmd_t *clicmd_array_end = (void *)&_clicmd_array_end;

  if(!strcmp(c->cl_argv[0], "help")) {
    cli_printf(c, "Available commands:\n");
    for(; clicmd != clicmd_array_end; clicmd++) {
      cli_printf(c, "\t%s\n", clicmd->cmd);
    }
    return;
  }

  for(; clicmd != clicmd_array_end; clicmd++) {
    if(!strcmp(c->cl_argv[0], clicmd->cmd)) {
      error_t err = clicmd->dispatch(c, argc, c->cl_argv);

      if(err) {
        cli_printf(c, "! Error: %s\n", error_to_string(err));
      }
      return;
    }
  }
  cli_printf(c, "No such command\n");
}

#define ESCAPE 27
#define OPENING_BRACKET 91
#define UP 65
#define DOWN 66
#define RIGHT 67
#define LEFT 68

/* VT100 escape code */
static const char code_cursor_left[] = {ESCAPE, OPENING_BRACKET, LEFT, 0};
static const char code_cursor_right[] = {ESCAPE, OPENING_BRACKET, RIGHT, 0};
static const char code_clear_line[] = {ESCAPE, OPENING_BRACKET, '2', 'K', 0};

static size_t
cli_prompt(cli_t *cl, char promptchar)
{
  size_t len = 0;
  cli_printf(cl, code_clear_line);
  cli_printf(cl, "\r");
  mutex_lock(&hostname_mutex);
  if(hostname[0]) {
    len = cli_printf(cl, "%s", hostname);
  }
  mutex_unlock(&hostname_mutex);
  len += cli_printf(cl, "%c ", promptchar);
  cli_printf(cl, NULL);
  return len;
}

static void
cli_set_cursor_pos(cli_t *cl, int cursor_pos, char promptchar)
{
  size_t len = cli_prompt(cl, promptchar);
  cl->cl_pos = cursor_pos;
  cli_printf(cl, "%s", cl->cl_buf);
  // Set cursor position
  cli_printf(cl, "\033[%zd`", cl->cl_pos + len + 1);
}

static void
cli_input_char(cli_t *cl, char c, char promptchar)
{
  static enum {
    RAW,
    ESCAPED,
    BRACKET } state = RAW;
  switch((uint8_t)c) {
  case 127:
    // Delete code (backspace function)
    if(cl->cl_pos) {
      cl->cl_pos--;
      int i = cl->cl_pos + 1;
      while (cl->cl_buf[i]) {
        cl->cl_buf[i - 1] = cl->cl_buf[i];
        i++;
      }
      cl->cl_buf[i-1] = '\0';
      cli_set_cursor_pos(cl, cl->cl_pos, promptchar);
    }
    break;
  case 4: // Backspace code (delete function)
  case 8: // Ctrl-d
    if(cl->cl_pos >= 0) {
      int i = cl->cl_pos + 1;
      while (cl->cl_buf[i]) {
        cl->cl_buf[i - 1] = cl->cl_buf[i];
        i++;
      }
      cl->cl_buf[i-1] = '\0';
      cli_set_cursor_pos(cl, cl->cl_pos, promptchar);
    }
    break;
  case 1: // Ctrl-a (QEMU eats this, do it twice to send it)
    cli_set_cursor_pos(cl, 0, promptchar);
    break;
  case 5: // Ctrl-e
    cli_set_cursor_pos(cl, strlen(cl->cl_buf), promptchar);
    break;
  case ESCAPE:
    state = ESCAPED;
    break;
  case OPENING_BRACKET:
    if (state == ESCAPED) {
      state = BRACKET;
      break;
    }
    // Fallthrough
  case UP:
    if (state == BRACKET) {
      char *str = history_get_current_line();
      history_up();
      memset(cl->cl_buf, 0, sizeof cl->cl_buf);
      if (str)
        strlcpy(cl->cl_buf, str, sizeof cl->cl_buf - 1);
      cli_prompt(cl, promptchar);
      cli_printf(cl, "%s", cl->cl_buf);
      cl->cl_pos = strlen(cl->cl_buf);
      free(str);
      break;
    }
    // Fallthrough
  case DOWN:
    if (state == BRACKET) {
      char *str = history_get_current_line();
      history_down();
      memset(cl->cl_buf, 0, sizeof cl->cl_buf);
      if (str)
        strlcpy(cl->cl_buf, str, sizeof cl->cl_buf - 1);
      cli_prompt(cl, promptchar);
      cli_printf(cl, "%s", cl->cl_buf);
      cl->cl_pos = strlen(cl->cl_buf);
      free(str);
      break;
    }
    // Fallthrough
  case RIGHT:
    if (state == BRACKET) {
      cli_printf(cl, code_cursor_right);
      if (cl->cl_pos < sizeof cl->cl_buf)
        cl->cl_pos++;
      break;
    }
    // Fallthrough
  case LEFT:
    if (state == BRACKET) {
      cli_printf(cl, code_cursor_left);
      cl->cl_pos--;
      if (cl->cl_pos < 0)
        cl->cl_pos = 0;
    break;
    }
  case 10:
  case 13:
    cli_printf(cl, "\r\n");
    if(cl->cl_buf[0])
      history_add(cl->cl_buf);
    dispatch_command(cl, cl->cl_buf);
    cl->cl_pos = 0;
    memset(cl->cl_buf, 0, sizeof cl->cl_buf);
    cli_prompt(cl, promptchar);
    state = RAW;
    break;
  default:
    //    printf("\n\nGot code %d\n", c);
    if(cl->cl_pos < CLI_LINE_BUF_SIZE - 1) {
      if (cl->cl_buf[cl->cl_pos]) {
        // Insert mode
        int i = strlen(cl->cl_buf);
        cl->cl_buf[i+1] = '\0';
        while (i > cl->cl_pos) {
          i--;
          cl->cl_buf[i+1] = cl->cl_buf[i];
        }
        cl->cl_buf[cl->cl_pos++] = c;
        cli_set_cursor_pos(cl, cl->cl_pos, promptchar);
      } else {
        cl->cl_buf[cl->cl_pos++] = c;
        cli_printf(cl, "%c", c);
      }
    }
    state = RAW;
    break;
  }
  cli_printf(cl, NULL);
}


int
cli_getc(struct cli *cli, int wait)
{
  stream_t *s = cli->cl_stream;
  if(s->vtable->read == NULL)
    return ERR_NOT_IMPLEMENTED;

  char c;
  int r = stream_read(s, &c, 1, !!wait);
  if(r == 0)
    return ERR_NOT_READY;
  if(r < 0)
    return r;
  return c;
}

int
cli_on_stream(stream_t *s, char promptchar)
{
  cli_t cli = {
    .cl_stream = s
  };

  stream_write(s, "\n", 1, STREAM_WRITE_WAIT_DTR);
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
