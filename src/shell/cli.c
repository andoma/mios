#ifdef CLI_STANDALONE

// ====================================================================
// Standalone mode - no MIOS dependencies
// Run tests:    make cli_test
// Interactive:  make cli_run
// ====================================================================

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <termios.h>

#include "cli_edit.h"

#define CLI_MAX_ARGC 10

typedef int error_t;
#define ERR_INVALID_ARGS (-23)

typedef struct cli {
} cli_t;

typedef struct cli_cmd {
  const char *cmd;
  error_t (*dispatch)(cli_t *cli, int argc, char **argv);
} cli_cmd_t;


#ifdef linux

size_t
strlcat(char *dst, const char *src, size_t dstsize)
{
  size_t dstlen, srclen, i;

  for(dstlen = 0; dstlen < dstsize && dst[dstlen] != '\0'; dstlen++);

  for(srclen = 0; src[srclen] != '\0'; srclen++);

  if(dstlen == dstsize)
    return dstsize + srclen;

  for(i = 0; i + 1 < dstsize - dstlen && src[i] != '\0'; i++)
    dst[dstlen + i] = src[i];

  dst[dstlen + i] = '\0';
  return dstlen + srclen;
}
#endif

// ====================================================================
// Output capture — cli_printf writes to a buffer during tests,
// to stdout during interactive mode.
// ====================================================================

static char  test_output[4096];
static int   test_output_pos;
static int   interactive_mode;

static void
test_output_reset(void)
{
  test_output[0] = '\0';
  test_output_pos = 0;
}

// Returns 1 if needle is found in test_output
static int
test_output_contains(const char *needle)
{
  return strstr(test_output, needle) != NULL;
}

static int
cli_printf(cli_t *cli, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

static int
cli_printf(cli_t *cli, const char *fmt, ...)
{
  (void)cli;
  va_list ap;
  va_start(ap, fmt);
  int r;
  if(interactive_mode) {
    r = vprintf(fmt, ap);
  } else {
    int avail = (int)sizeof(test_output) - test_output_pos - 1;
    if(avail < 0) avail = 0;
    r = vsnprintf(test_output + test_output_pos, avail, fmt, ap);
    if(r > 0 && r <= avail)
      test_output_pos += r;
    else if(r > 0)
      test_output_pos = (int)sizeof(test_output) - 1;
  }
  va_end(ap);
  return r;
}

static void
cli_flush(cli_t *cli)
{
  (void)cli;
  if(interactive_mode)
    fflush(stdout);
}


// ====================================================================
// Test command dispatch tracking
// ====================================================================

static char last_dispatched_cmd[CLI_LINE_BUF_SIZE];
static int  last_dispatched_argc;
static char last_dispatched_args[CLI_LINE_BUF_SIZE];

static void
dispatch_tracking_reset(void)
{
  last_dispatched_cmd[0] = '\0';
  last_dispatched_argc = 0;
  last_dispatched_args[0] = '\0';
}

static error_t
cmd_dummy(cli_t *c, int argc, char **argv)
{
  (void)c;
  snprintf(last_dispatched_cmd, sizeof(last_dispatched_cmd), "%s", argv[0]);
  last_dispatched_argc = argc;
  last_dispatched_args[0] = '\0';
  for(int i = 1; i < argc; i++) {
    if(i > 1)
      strlcat(last_dispatched_args, " ", sizeof(last_dispatched_args));
    strlcat(last_dispatched_args, argv[i], sizeof(last_dispatched_args));
  }
  return 0;
}

static const cli_cmd_t test_commands[] = {
  {"arp",                cmd_dummy},
  {"bootflash_erase",    cmd_dummy},
  {"bootflash_install",  cmd_dummy},
  {"bootflash_setchain", cmd_dummy},
  {"cat",                cmd_dummy},
  {"date",               cmd_dummy},
  {"dev",                cmd_dummy},
  {"ls",                 cmd_dummy},
  {"mem",                cmd_dummy},
  {"metric",             cmd_dummy},
  {"net_ipv4_show",      cmd_dummy},
  {"net_ipv4_status",    cmd_dummy},
  {"net_ble_scan",       cmd_dummy},
  {"ps",                 cmd_dummy},
  {"reset",              cmd_dummy},
  {"show_devices",       cmd_dummy},
  {"show_gpio",          cmd_dummy},
  {"show_tasks",         cmd_dummy},
  {"sysinfo",            cmd_dummy},
  {"uptime",             cmd_dummy},
};

#define CMD_ARRAY_BEGIN test_commands
#define CMD_ARRAY_END \
  (test_commands + sizeof(test_commands) / sizeof(test_commands[0]))

static const char *
error_to_string(error_t e)
{
  (void)e;
  return "ERROR";
}

// Simple history for standalone mode
#define HIST_MAX 8
static char *hist_lines[HIST_MAX];
static int hist_count;
static int hist_pos;

static void
history_reset(void)
{
  for(int i = 0; i < HIST_MAX; i++) {
    free(hist_lines[i]);
    hist_lines[i] = NULL;
  }
  hist_count = 0;
  hist_pos = 0;
}

static void
history_add(const char *line)
{
  if(hist_count > 0 &&
     !strcmp(hist_lines[(hist_count - 1) % HIST_MAX], line))
    return;
  int idx = hist_count % HIST_MAX;
  free(hist_lines[idx]);
  hist_lines[idx] = strdup(line);
  hist_count++;
  hist_pos = hist_count;
}

static void
history_up(void)
{
  if(hist_pos > 0 && hist_pos > hist_count - HIST_MAX)
    hist_pos--;
}

static void
history_down(void)
{
  if(hist_pos < hist_count)
    hist_pos++;
}

static char *
history_get_current_line(void)
{
  if(hist_pos >= hist_count)
    return NULL;
  return strdup(hist_lines[hist_pos % HIST_MAX]);
}

static size_t
cli_prompt(cli_t *cl, char promptchar)
{
  size_t len;
  len = cli_printf(cl, "\r\033[2Kmios%c ", promptchar);
  cli_flush(cl);
  return len - 4; // subtract \r\033[2K which are non-visible
}

#else

// ====================================================================
// MIOS mode
// ====================================================================

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/cli.h>
#include <mios/mios.h>
#include <mios/error.h>
#include <mios/version.h>
#include <mios/hostname.h>

#include "cli_edit.h"
#include "history.h"

#define CLI_MAX_ARGC 10

extern unsigned long _clicmd_array_begin;
extern unsigned long _clicmd_array_end;

#define CMD_ARRAY_BEGIN ((const cli_cmd_t *)&_clicmd_array_begin)
#define CMD_ARRAY_END   ((const cli_cmd_t *)&_clicmd_array_end)

static const char errmsg[] = {
  "OK\0"
  "NOT_IMPLEMENTED\0"
  "TIMEOUT\0"
  "OPERATION_FAILED\0"
  "TX_FAULT\0"
  "RX_FAULT\0"
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

static size_t
cli_prompt(cli_t *cl, char promptchar)
{
  size_t len = 0;
  cli_printf(cl, "\r\033[2K");
  mutex_lock(&hostname_mutex);
  if(hostname[0]) {
    len = cli_printf(cl, "%s", hostname);
  }
  mutex_unlock(&hostname_mutex);
  len += cli_printf(cl, "%c ", promptchar);
  cli_flush(cl);
  return len;
}

#endif // CLI_STANDALONE


// ====================================================================
// Common code
// ====================================================================

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


// Compare command name against argv[0..n-1] joined with underscores.
// Returns pointer into cmd right after the matched portion, or NULL.
static const char *
match_argv(const char *cmd, char **argv, int n)
{
  const char *p = cmd;
  for(int i = 0; i < n; i++) {
    if(i > 0) {
      if(*p != '_')
        return NULL;
      p++;
    }
    const char *t = argv[i];
    while(*t) {
      if(*p != *t)
        return NULL;
      p++;
      t++;
    }
  }
  return p;
}


// Check if command name starts with editor buffer content,
// treating spaces in buf as underscores.
static int
prefix_match(const char *cmd, const char *buf, int len)
{
  for(int i = 0; i < len; i++) {
    char bc = (buf[i] == ' ') ? '_' : buf[i];
    if(cmd[i] != bc)
      return 0;
  }
  return 1;
}


static void show_help(cli_t *c, int argc, char **argv);

static void
dispatch_command(cli_t *c, char *line)
{
  char *argv[CLI_MAX_ARGC];
  int argc = tokenize(line, argv, CLI_MAX_ARGC);
  if(argc == 0)
    return;

  // Handle "help" command
  if(!strcmp(argv[0], "help")) {
    show_help(c, argc, argv);
    return;
  }

  const cli_cmd_t *begin = CMD_ARRAY_BEGIN;
  const cli_cmd_t *end = CMD_ARRAY_END;

  // Try progressively joining more tokens with underscores
  for(int join = 1; join <= argc; join++) {
    for(const cli_cmd_t *p = begin; p != end; p++) {
      const char *r = match_argv(p->cmd, argv, join);
      if(r && *r == '\0') {
        // Exact match — rebuild argv
        argv[0] = (char *)p->cmd;
        int new_argc = argc - join + 1;
        for(int i = 1; i < new_argc; i++)
          argv[i] = argv[join + i - 1];

        error_t err = p->dispatch(c, new_argc, argv);
        if(err)
          cli_printf(c, "! Error: %s\n", error_to_string(err));
        return;
      }
    }
  }

  // Check if it could be a valid group prefix
  for(const cli_cmd_t *p = begin; p != end; p++) {
    const char *r = match_argv(p->cmd, argv, argc);
    if(r && *r == '_') {
      cli_printf(c, "Incomplete command. Try 'help %s'.\n", argv[0]);
      return;
    }
  }

  cli_printf(c, "Unknown command\n");
}


// ====================================================================
// Help system
// ====================================================================

static void
show_help(cli_t *c, int argc, char **argv)
{
  const cli_cmd_t *begin = CMD_ARRAY_BEGIN;
  const cli_cmd_t *end = CMD_ARRAY_END;

  int plen = 0;
  const cli_cmd_t *first = begin;

  if(argc > 1) {
    // Find first command matching the group prefix (sorted array)
    for(const cli_cmd_t *p = begin; p != end; p++) {
      const char *r = match_argv(p->cmd, argv + 1, argc - 1);
      if(r && *r == '_') {
        first = p;
        plen = r - p->cmd + 1; // include trailing underscore
        break;
      }
    }
    if(plen == 0)
      return; // No matching group
  }

  const char *prev_seg = NULL;
  int prev_len = 0;

  for(const cli_cmd_t *p = first; p != end; p++) {
    if(strncmp(p->cmd, first->cmd, plen))
      break; // sorted array — no more matches

    const char *suffix = p->cmd + plen;

    // Extract next segment (up to underscore or end)
    const char *seg_end = suffix;
    while(*seg_end && *seg_end != '_')
      seg_end++;

    int seg_len = seg_end - suffix;
    if(seg_len == 0)
      continue;

    // Deduplicate using pointer into command strings (no stack buffer)
    if(seg_len == prev_len && prev_seg &&
       !strncmp(suffix, prev_seg, seg_len))
      continue;
    prev_seg = suffix;
    prev_len = seg_len;

    cli_printf(c, "  %.*s\n", seg_len, suffix);
  }
}


// ====================================================================
// Tab completion
// ====================================================================

static void
do_completion(cli_t *c, cli_ed_t *ed, int list_only)
{
  const cli_cmd_t *begin = CMD_ARRAY_BEGIN;
  const cli_cmd_t *end = CMD_ARRAY_END;
  int plen = ed->len;

  int help_match = (plen <= 4) && !strncmp("help", ed->buf, plen);

  // Pass 1: count matches and compute longest common prefix.
  const cli_cmd_t *first = NULL;
  int nmatch = 0;
  int common = 0;

  for(const cli_cmd_t *p = begin; p != end; p++) {
    if(!prefix_match(p->cmd, ed->buf, plen))
      continue;
    if(!first) {
      first = p;
      common = strlen(p->cmd);
    } else {
      int i = plen;
      while(i < common && p->cmd[i] == first->cmd[i])
        i++;
      common = i;
    }
    nmatch++;
  }

  if(nmatch == 0 && !help_match)
    return;

  if(!list_only) {
    // Also constrain common prefix against "help" if it matches
    if(help_match) {
      if(nmatch == 0) {
        common = 4;
      } else {
        int i = plen;
        while(i < common && i < 4 && first->cmd[i] == "help"[i])
          i++;
        common = i;
      }
    }

    if(common > plen) {
      const char *src = nmatch > 0 ? first->cmd : "help";

      for(int i = plen; i < common; i++) {
        char ch = (src[i] == '_') ? ' ' : src[i];
        cli_ed_insert(ed, &ch, 1);
      }

      // Add trailing space if we completed to a full command or group boundary
      if(nmatch == 1 && first->cmd[common] == '\0') {
        cli_ed_insert(ed, " ", 1);
      } else if(nmatch == 0 && common == 4) {
        cli_ed_insert(ed, " ", 1);
      } else if(nmatch > 0 && first->cmd[common] == '_') {
        cli_ed_insert(ed, " ", 1);
      }
      return;
    }
  }

  // Can't extend further — list alternatives (pass 2)
  cli_printf(c, "\n");

  // Find word boundary: position after last space in editor buffer
  int word_start = plen;
  while(word_start > 0 && ed->buf[word_start - 1] != ' ')
    word_start--;

  const char *prev_seg = NULL;
  int prev_len = 0;

  if(help_match && plen <= 4)
    cli_printf(c, "  help\n");

  for(const cli_cmd_t *p = begin; p != end; p++) {
    if(!prefix_match(p->cmd, ed->buf, plen))
      continue;

    const char *suffix = p->cmd + word_start;
    const char *seg_end = suffix;
    while(*seg_end && *seg_end != '_')
      seg_end++;

    int seg_len = seg_end - suffix;
    if(seg_len == 0)
      continue;

    // Deduplicate using pointer into command strings (no stack buffer)
    if(seg_len == prev_len && prev_seg &&
       !strncmp(suffix, prev_seg, seg_len))
      continue;
    prev_seg = suffix;
    prev_len = seg_len;

    cli_printf(c, "  %.*s\n", seg_len, suffix);
  }
}


// ====================================================================
// Line rendering
// ====================================================================

static void
redraw_line(cli_t *c, cli_ed_t *ed, char promptchar)
{
  size_t prompt_len = cli_prompt(c, promptchar);
  cli_printf(c, "%s", ed->buf);
  // Position cursor: prompt_len + ed->pos + 1 (1-based column)
  cli_printf(c, "\033[%dG", (int)(prompt_len + ed->pos + 1));
  cli_flush(c);
}


// ====================================================================
// CLI main loop
// ====================================================================

static void
cli_loop(cli_t *c, char promptchar,
         int (*getchar_fn)(cli_t *c, int wait))
{
  cli_ed_t ed;
  cli_ed_init(&ed);

  redraw_line(c, &ed, promptchar);

  while(1) {
    int ch = getchar_fn(c, 1);
    if(ch < 0)
      return;

    int16_t old_pos = ed.pos;
    int16_t old_len = ed.len;

    cli_ed_event_t ev = cli_ed_input(&ed, ch);
    switch(ev) {

    case CLI_ED_ENTER:
      cli_printf(c, "\r\n");
      if(ed.buf[0]) {
        history_add(ed.buf);
        dispatch_command(c, ed.buf); // tokenizes ed.buf in-place
      }
      cli_ed_clear(&ed);
      redraw_line(c, &ed, promptchar);
      break;

    case CLI_ED_TAB:
      do_completion(c, &ed, 0);
      redraw_line(c, &ed, promptchar);
      break;

    case CLI_ED_HELP:
      // Show context-sensitive help for current input
      // Use current buffer as help prefix
      if(ed.len > 0) {
        do_completion(c, &ed, 1);
      } else {
        cli_printf(c, "\n");
        char *help_argv[] = { "help" };
        show_help(c, 1, help_argv);
      }
      redraw_line(c, &ed, promptchar);
      break;

    case CLI_ED_HIST_UP: {
      history_up();
      char *str = history_get_current_line();
      cli_ed_set(&ed, str);
      free(str);
      redraw_line(c, &ed, promptchar);
      break;
    }

    case CLI_ED_HIST_DOWN: {
      history_down();
      char *str = history_get_current_line();
      cli_ed_set(&ed, str);
      free(str);
      redraw_line(c, &ed, promptchar);
      break;
    }

    case CLI_ED_CLEAR:
      // Clear screen and redraw
      cli_printf(c, "\033[2J\033[H");
      redraw_line(c, &ed, promptchar);
      break;

    case CLI_ED_CANCEL:
      cli_printf(c, "\r\n");
      cli_ed_clear(&ed);
      redraw_line(c, &ed, promptchar);
      break;

    case CLI_ED_EOF:
      cli_printf(c, "\r\n");
      return;

    case CLI_ED_NONE:
      if(ed.len == old_len && ed.pos == old_pos) {
        // Nothing changed (e.g. mid-escape sequence) — skip redraw
      } else if(ed.pos == ed.len && ed.len == old_len + 1 &&
                ed.pos == old_pos + 1) {
        // Character appended at end — just echo it
        cli_printf(c, "%c", ed.buf[ed.pos - 1]);
        cli_flush(c);
      } else if(ed.pos == ed.len && ed.len == old_len - 1 &&
                ed.pos == old_pos - 1) {
        // Backspace at end — erase one character
        cli_printf(c, "\b \b");
        cli_flush(c);
      } else {
        redraw_line(c, &ed, promptchar);
      }
      break;
    }
  }
}


#ifdef CLI_STANDALONE

// ====================================================================
// Test framework
// ====================================================================

static int test_count;
static int test_fail;

#define CHECK(cond) do { \
  test_count++; \
  if(!(cond)) { \
    fprintf(stderr, "  FAIL line %d: %s\n", __LINE__, #cond); \
    test_fail++; \
  } \
} while(0)

// Feed a string of characters into the editor, handling events
// the same way cli_loop does (completion, help, history).
static void
feed_input(cli_t *c, cli_ed_t *ed, const char *input)
{
  for(const char *p = input; *p; p++) {
    cli_ed_event_t ev = cli_ed_input(ed, *p);
    switch(ev) {
    case CLI_ED_TAB:
      do_completion(c, ed, 0);
      break;
    case CLI_ED_HELP:
      do_completion(c, ed, 1);
      break;
    case CLI_ED_ENTER:
      if(ed->buf[0]) {
        history_add(ed->buf);
        dispatch_command(c, ed->buf);
      }
      cli_ed_clear(ed);
      break;
    case CLI_ED_HIST_UP: {
      history_up();
      char *str = history_get_current_line();
      cli_ed_set(ed, str);
      free(str);
      break;
    }
    case CLI_ED_HIST_DOWN: {
      history_down();
      char *str = history_get_current_line();
      cli_ed_set(ed, str);
      free(str);
      break;
    }
    case CLI_ED_CANCEL:
      cli_ed_clear(ed);
      break;
    default:
      break;
    }
  }
}

// Convenience: reset all state for a fresh test
static void
test_reset(cli_t *c, cli_ed_t *ed)
{
  memset(c, 0, sizeof(*c));
  cli_ed_init(ed);
  test_output_reset();
  dispatch_tracking_reset();
  history_reset();
}


// ====================================================================
// Editor tests
// ====================================================================

static void
test_editor_basic_typing(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);

  for(const char *p = "hello"; *p; p++)
    cli_ed_input(&ed, *p);

  CHECK(!strcmp(ed.buf, "hello"));
  CHECK(ed.pos == 5);
  CHECK(ed.len == 5);
}

static void
test_editor_backspace(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);

  for(const char *p = "hello"; *p; p++)
    cli_ed_input(&ed, *p);
  cli_ed_input(&ed, 127); // backspace
  cli_ed_input(&ed, 127);

  CHECK(!strcmp(ed.buf, "hel"));
  CHECK(ed.pos == 3);
  CHECK(ed.len == 3);
}

static void
test_editor_ctrl_a_e(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello world");

  cli_ed_input(&ed, 1); // Ctrl-A
  CHECK(ed.pos == 0);

  cli_ed_input(&ed, 5); // Ctrl-E
  CHECK(ed.pos == ed.len);
  CHECK(ed.pos == 11);
}

static void
test_editor_ctrl_k(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello world");
  ed.pos = 5;

  cli_ed_input(&ed, 11); // Ctrl-K
  CHECK(!strcmp(ed.buf, "hello"));
  CHECK(ed.len == 5);
  CHECK(ed.pos == 5);
}

static void
test_editor_ctrl_u(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello world");

  cli_ed_input(&ed, 21); // Ctrl-U
  CHECK(!strcmp(ed.buf, ""));
  CHECK(ed.len == 0);
  CHECK(ed.pos == 0);
}

static void
test_editor_ctrl_w(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello world");

  cli_ed_input(&ed, 23); // Ctrl-W
  CHECK(!strcmp(ed.buf, "hello "));
  CHECK(ed.pos == 6);

  cli_ed_input(&ed, 23); // Ctrl-W again
  CHECK(!strcmp(ed.buf, ""));
  CHECK(ed.pos == 0);
}

static void
test_editor_ctrl_w_mid_word(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello world foo");
  ed.pos = 11; // cursor at 'd' of "world"

  // Ctrl-W should kill "worl" back to space
  cli_ed_input(&ed, 23);
  CHECK(!strcmp(ed.buf, "hello  foo"));
}

static void
test_editor_insert_middle(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "helo");
  ed.pos = 3; // between 'l' and 'o'

  cli_ed_input(&ed, 'l');
  CHECK(!strcmp(ed.buf, "hello"));
  CHECK(ed.pos == 4);
  CHECK(ed.len == 5);
}

static void
test_editor_arrow_keys(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "abc");

  // Left arrow: ESC [ D
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, '[');
  cli_ed_input(&ed, 'D');
  CHECK(ed.pos == 2);

  // Right arrow: ESC [ C
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, '[');
  cli_ed_input(&ed, 'C');
  CHECK(ed.pos == 3);

  // Left beyond start
  ed.pos = 0;
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, '[');
  cli_ed_input(&ed, 'D');
  CHECK(ed.pos == 0);
}

static void
test_editor_home_end(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello");
  ed.pos = 3;

  // Home: ESC [ H
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, '[');
  cli_ed_input(&ed, 'H');
  CHECK(ed.pos == 0);

  // End: ESC [ F
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, '[');
  cli_ed_input(&ed, 'F');
  CHECK(ed.pos == 5);
}

static void
test_editor_delete_key(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello");
  ed.pos = 2;

  // Delete: ESC [ 3 ~
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, '[');
  cli_ed_input(&ed, '3');
  cli_ed_input(&ed, '~');
  CHECK(!strcmp(ed.buf, "helo"));
  CHECK(ed.pos == 2);
}

static void
test_editor_ctrl_d_delete(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello");
  ed.pos = 0;

  cli_ed_input(&ed, 4); // Ctrl-D deletes char at cursor
  CHECK(!strcmp(ed.buf, "ello"));
  CHECK(ed.pos == 0);
}

static void
test_editor_ctrl_d_eof(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);

  cli_ed_event_t ev = cli_ed_input(&ed, 4);
  CHECK(ev == CLI_ED_EOF);
}

static void
test_editor_word_movement(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello world foo");
  ed.pos = 0;

  // Alt-F (word forward): ESC f
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, 'f');
  CHECK(ed.pos == 6); // past "hello "

  // Alt-B (word backward): ESC b
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, 'b');
  CHECK(ed.pos == 0);
}

static void
test_editor_ctrl_left_right(void)
{
  cli_ed_t ed;
  cli_ed_init(&ed);
  cli_ed_set(&ed, "hello world foo");
  ed.pos = 0;

  // Ctrl-Right: ESC [ 1 ; 5 C
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, '[');
  cli_ed_input(&ed, '1');
  cli_ed_input(&ed, ';');
  cli_ed_input(&ed, '5');
  cli_ed_input(&ed, 'C');
  CHECK(ed.pos == 6);

  // Ctrl-Left: ESC [ 1 ; 5 D
  cli_ed_input(&ed, 27);
  cli_ed_input(&ed, '[');
  cli_ed_input(&ed, '1');
  cli_ed_input(&ed, ';');
  cli_ed_input(&ed, '5');
  cli_ed_input(&ed, 'D');
  CHECK(ed.pos == 0);
}


// ====================================================================
// Completion tests
// ====================================================================

static void
test_complete_unambiguous(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  // Type "boo" then complete
  for(const char *p = "boo"; *p; p++)
    cli_ed_input(&ed, *p);

  test_output_reset();
  do_completion(&c, &ed, 0);
  CHECK(!strcmp(ed.buf, "bootflash "));
  CHECK(ed.pos == 10);
}

static void
test_complete_single_match(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  // "reset" is unique from "re"
  cli_ed_set(&ed, "re");
  test_output_reset();
  do_completion(&c, &ed, 0);
  CHECK(!strcmp(ed.buf, "reset "));
}

static void
test_complete_subcommand(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  cli_ed_set(&ed, "bootflash e");
  test_output_reset();
  do_completion(&c, &ed, 0);
  CHECK(!strcmp(ed.buf, "bootflash erase "));
}

static void
test_complete_group_boundary(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  // "sh" should complete to "show " (all sh* commands are show_*)
  cli_ed_set(&ed, "sh");
  test_output_reset();
  do_completion(&c, &ed, 0);
  CHECK(!strcmp(ed.buf, "show "));
}

static void
test_complete_list_alternatives(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  // "bootflash " has 3 subcommands — can't extend, should list
  cli_ed_set(&ed, "bootflash ");
  test_output_reset();
  do_completion(&c, &ed, 0);
  // Buffer unchanged
  CHECK(!strcmp(ed.buf, "bootflash "));
  // Output should contain the alternatives
  CHECK(test_output_contains("erase"));
  CHECK(test_output_contains("install"));
  CHECK(test_output_contains("setchain"));
}

static void
test_complete_multilevel(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  // "net " has ipv4 and ble — can't extend further
  cli_ed_set(&ed, "net ");
  test_output_reset();
  do_completion(&c, &ed, 0);
  CHECK(!strcmp(ed.buf, "net "));
  CHECK(test_output_contains("ipv4"));
  CHECK(test_output_contains("ble"));
}

static void
test_complete_multilevel_extend(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  // "net i" extends to "net ipv4 s" (both show/status start with 's')
  cli_ed_set(&ed, "net i");
  test_output_reset();
  do_completion(&c, &ed, 0);
  CHECK(!strcmp(ed.buf, "net ipv4 s"));
}

static void
test_complete_partial_word_listing(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  // "net ipv4 s" — matches show and status, can't extend
  // should list "show" and "status" (full words, not "how" / "tatus")
  cli_ed_set(&ed, "net ipv4 s");
  test_output_reset();
  do_completion(&c, &ed, 0);
  CHECK(!strcmp(ed.buf, "net ipv4 s"));
  CHECK(test_output_contains("  show\n"));
  CHECK(test_output_contains("  status\n"));
  // Must not contain bare fragments (would mean word_start backtrack failed)
  CHECK(!test_output_contains("  how\n"));
  CHECK(!test_output_contains("  tatus\n"));
}

static void
test_complete_no_match(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  cli_ed_set(&ed, "zzz");
  test_output_reset();
  do_completion(&c, &ed, 0);
  // Buffer unchanged
  CHECK(!strcmp(ed.buf, "zzz"));
}

static void
test_complete_help_word(void)
{
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);

  // "hel" should complete to "help "
  cli_ed_set(&ed, "hel");
  test_output_reset();
  do_completion(&c, &ed, 0);
  CHECK(!strcmp(ed.buf, "help "));
}


// ====================================================================
// Help tests
// ====================================================================

static void
test_help_toplevel(void)
{
  cli_t c = {};
  char *argv[] = { "help" };

  test_output_reset();
  show_help(&c, 1, argv);
  // Should show groups and standalone commands
  CHECK(test_output_contains("bootflash"));
  CHECK(test_output_contains("reset"));
  CHECK(test_output_contains("show"));
  CHECK(test_output_contains("net"));
  // Should NOT show individual subcommands at top level
  CHECK(!test_output_contains("erase"));
  CHECK(!test_output_contains("devices"));
  CHECK(!test_output_contains("ipv4"));
}

static void
test_help_group(void)
{
  cli_t c = {};
  char *argv[] = { "help", "bootflash" };

  test_output_reset();
  show_help(&c, 2, argv);
  CHECK(test_output_contains("erase"));
  CHECK(test_output_contains("install"));
  CHECK(test_output_contains("setchain"));
}

static void
test_help_multilevel(void)
{
  cli_t c = {};
  char *argv[] = { "help", "net" };

  test_output_reset();
  show_help(&c, 2, argv);
  CHECK(test_output_contains("ipv4"));
  CHECK(test_output_contains("ble"));
  // Should NOT show the leaf commands
  CHECK(!test_output_contains("show"));
  CHECK(!test_output_contains("scan"));
}

static void
test_help_deep(void)
{
  cli_t c = {};
  char *argv[] = { "help", "net", "ipv4" };

  test_output_reset();
  show_help(&c, 3, argv);
  CHECK(test_output_contains("show"));
  CHECK(test_output_contains("status"));
}


// ====================================================================
// Dispatch tests
// ====================================================================

static void
test_dispatch_simple(void)
{
  cli_t c = {};
  dispatch_tracking_reset();
  char line[] = "reset";
  dispatch_command(&c, line);
  CHECK(!strcmp(last_dispatched_cmd, "reset"));
  CHECK(last_dispatched_argc == 1);
}

static void
test_dispatch_grouped(void)
{
  cli_t c = {};
  dispatch_tracking_reset();
  char line[] = "bootflash erase";
  dispatch_command(&c, line);
  CHECK(!strcmp(last_dispatched_cmd, "bootflash_erase"));
  CHECK(last_dispatched_argc == 1);
}

static void
test_dispatch_grouped_with_args(void)
{
  cli_t c = {};
  dispatch_tracking_reset();
  char line[] = "bootflash install file.bin";
  dispatch_command(&c, line);
  CHECK(!strcmp(last_dispatched_cmd, "bootflash_install"));
  CHECK(last_dispatched_argc == 2);
  CHECK(!strcmp(last_dispatched_args, "file.bin"));
}

static void
test_dispatch_multilevel(void)
{
  cli_t c = {};
  dispatch_tracking_reset();
  char line[] = "net ipv4 show";
  dispatch_command(&c, line);
  CHECK(!strcmp(last_dispatched_cmd, "net_ipv4_show"));
  CHECK(last_dispatched_argc == 1);
}

static void
test_dispatch_multilevel_with_args(void)
{
  cli_t c = {};
  dispatch_tracking_reset();
  char line[] = "net ble scan 100";
  dispatch_command(&c, line);
  CHECK(!strcmp(last_dispatched_cmd, "net_ble_scan"));
  CHECK(last_dispatched_argc == 2);
  CHECK(!strcmp(last_dispatched_args, "100"));
}

static void
test_dispatch_unknown(void)
{
  cli_t c = {};
  dispatch_tracking_reset();
  test_output_reset();
  char line[] = "nonexistent";
  dispatch_command(&c, line);
  CHECK(last_dispatched_cmd[0] == '\0');
  CHECK(test_output_contains("Unknown command"));
}

static void
test_dispatch_incomplete_group(void)
{
  cli_t c = {};
  dispatch_tracking_reset();
  test_output_reset();
  char line[] = "show";
  dispatch_command(&c, line);
  CHECK(last_dispatched_cmd[0] == '\0');
  CHECK(test_output_contains("Incomplete command"));
}


// ====================================================================
// Integration tests — feed_input drives the full event loop
// ====================================================================

static void
test_integration_tab_then_execute(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  // Type "boo", tab complete, then "er", tab complete, then Enter
  feed_input(&c, &ed, "boo\x09" "er\x09\r");
  CHECK(!strcmp(last_dispatched_cmd, "bootflash_erase"));
}

static void
test_integration_help_via_enter(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  feed_input(&c, &ed, "help\r");
  CHECK(test_output_contains("bootflash"));
  CHECK(test_output_contains("show"));
  CHECK(test_output_contains("net"));
}

static void
test_integration_help_group_via_enter(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  feed_input(&c, &ed, "help bootflash\r");
  CHECK(test_output_contains("erase"));
  CHECK(test_output_contains("install"));
}

static void
test_integration_history(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  // Execute "reset", then use up arrow to recall it
  feed_input(&c, &ed, "reset\r");
  CHECK(!strcmp(last_dispatched_cmd, "reset"));

  dispatch_tracking_reset();
  // Up arrow: ESC [ A, then Enter
  feed_input(&c, &ed, "\x1b[A\r");
  CHECK(!strcmp(last_dispatched_cmd, "reset"));
}

static void
test_integration_ctrl_c_cancel(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  // Type something, Ctrl-C, then execute something else
  feed_input(&c, &ed, "bootflash");
  CHECK(!strcmp(ed.buf, "bootflash"));
  feed_input(&c, &ed, "\x03"); // Ctrl-C
  CHECK(!strcmp(ed.buf, ""));

  dispatch_tracking_reset();
  feed_input(&c, &ed, "reset\r");
  CHECK(!strcmp(last_dispatched_cmd, "reset"));
}

static void
test_integration_edit_and_execute(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  // Type "rese" + Ctrl-A + delete at cursor (Ctrl-D doesn't mean EOF
  // because line is non-empty) — actually let's use Ctrl-E + backspace
  // to test editing
  // Type "resett", backspace, enter
  feed_input(&c, &ed, "resett\x7f\r");
  CHECK(!strcmp(last_dispatched_cmd, "reset"));
}


// ====================================================================
// Test runner
// ====================================================================

typedef void (*test_fn)(void);

static const struct {
  const char *name;
  test_fn fn;
} all_tests[] = {
  // Editor tests
  {"editor: basic typing",          test_editor_basic_typing},
  {"editor: backspace",             test_editor_backspace},
  {"editor: ctrl-a/ctrl-e",         test_editor_ctrl_a_e},
  {"editor: ctrl-k kill to end",    test_editor_ctrl_k},
  {"editor: ctrl-u kill line",      test_editor_ctrl_u},
  {"editor: ctrl-w kill word",      test_editor_ctrl_w},
  {"editor: ctrl-w mid-word",       test_editor_ctrl_w_mid_word},
  {"editor: insert in middle",      test_editor_insert_middle},
  {"editor: arrow keys",            test_editor_arrow_keys},
  {"editor: home/end keys",         test_editor_home_end},
  {"editor: delete key",            test_editor_delete_key},
  {"editor: ctrl-d delete",         test_editor_ctrl_d_delete},
  {"editor: ctrl-d EOF on empty",   test_editor_ctrl_d_eof},
  {"editor: alt-f/b word movement", test_editor_word_movement},
  {"editor: ctrl-left/right",       test_editor_ctrl_left_right},

  // Completion tests
  {"complete: unambiguous prefix",  test_complete_unambiguous},
  {"complete: single match",        test_complete_single_match},
  {"complete: subcommand",          test_complete_subcommand},
  {"complete: group boundary",      test_complete_group_boundary},
  {"complete: list alternatives",   test_complete_list_alternatives},
  {"complete: multi-level list",    test_complete_multilevel},
  {"complete: multi-level extend",  test_complete_multilevel_extend},
  {"complete: partial word list",   test_complete_partial_word_listing},
  {"complete: no match",            test_complete_no_match},
  {"complete: help word",           test_complete_help_word},

  // Help tests
  {"help: top level",               test_help_toplevel},
  {"help: group",                   test_help_group},
  {"help: multi-level",             test_help_multilevel},
  {"help: deep",                    test_help_deep},

  // Dispatch tests
  {"dispatch: simple command",       test_dispatch_simple},
  {"dispatch: grouped command",      test_dispatch_grouped},
  {"dispatch: grouped with args",    test_dispatch_grouped_with_args},
  {"dispatch: multi-level",          test_dispatch_multilevel},
  {"dispatch: multi-level with args",test_dispatch_multilevel_with_args},
  {"dispatch: unknown command",      test_dispatch_unknown},
  {"dispatch: incomplete group",     test_dispatch_incomplete_group},

  // Integration tests
  {"integ: tab complete and execute",  test_integration_tab_then_execute},
  {"integ: help via enter",            test_integration_help_via_enter},
  {"integ: help group via enter",      test_integration_help_group_via_enter},
  {"integ: history recall",            test_integration_history},
  {"integ: ctrl-c cancel",            test_integration_ctrl_c_cancel},
  {"integ: edit and execute",          test_integration_edit_and_execute},
};

#define NUM_TESTS (int)(sizeof(all_tests) / sizeof(all_tests[0]))

static int
run_tests(void)
{
  test_count = 0;
  test_fail = 0;

  for(int i = 0; i < NUM_TESTS; i++) {
    int before = test_fail;
    all_tests[i].fn();
    if(test_fail == before)
      fprintf(stderr, "  OK   %s\n", all_tests[i].name);
  }

  fprintf(stderr, "\n%d checks, %d failed\n", test_count, test_fail);
  return test_fail ? 1 : 0;
}


// ====================================================================
// Interactive mode (run with -i flag)
// ====================================================================

static struct termios orig_termios;
static int raw_mode_set;

static void
disable_raw_mode(void)
{
  if(raw_mode_set)
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void
enable_raw_mode(void)
{
  tcgetattr(STDIN_FILENO, &orig_termios);
  raw_mode_set = 1;
  atexit(disable_raw_mode);

  struct termios raw = orig_termios;
  raw.c_iflag &= ~(ICRNL | IXON);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static int
sa_getchar(cli_t *c, int wait)
{
  (void)c;
  (void)wait;
  unsigned char ch;
  int r = read(STDIN_FILENO, &ch, 1);
  if(r <= 0)
    return -1;
  return ch;
}


// ====================================================================
// main
// ====================================================================

int
main(int argc, char **argv)
{
  if(argc > 1 && !strcmp(argv[1], "-i")) {
    // Interactive mode
    interactive_mode = 1;
    cli_t cli = {};
    enable_raw_mode();
    printf("\r\nCLI interactive test mode\r\n\r\n");
    cli_loop(&cli, '>', sa_getchar);
    printf("\r\n");
    return 0;
  }

  // Default: run tests
  return run_tests();
}

#else // MIOS mode

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

static int
mios_getchar(cli_t *c, int wait)
{
  return cli_getc(c, wait);
}

int
cli_on_stream(stream_t *s, char promptchar)
{
  cli_t cli = {
    .cl_stream = s
  };

  stream_write(s, "\n", 1, STREAM_WRITE_WAIT_DTR);
  mios_print_version(s);
  cli_loop(&cli, promptchar, mios_getchar);
  return 0;
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

#endif // CLI_STANDALONE
