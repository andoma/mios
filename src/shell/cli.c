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

#define ENABLE_CLI_EXTENDED_HELP

typedef struct cli_cmd {
  const char *cmd;
  error_t (*dispatch)(cli_t *cli, int argc, char **argv);
  const char *synopsis;
  const char *brief;
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

static error_t
cmd_bad_args(cli_t *c, int argc, char **argv)
{
  (void)c;
  snprintf(last_dispatched_cmd, sizeof(last_dispatched_cmd), "%s", argv[0]);
  last_dispatched_argc = argc;
  return ERR_INVALID_ARGS;
}

static error_t cmd_help(cli_t *c, int argc, char **argv);

static const cli_cmd_t test_commands[] = {
  {"arp",                cmd_dummy,    NULL, NULL},
  {"bootflash_erase",    cmd_dummy,    NULL, NULL},
  {"bootflash_install",  cmd_dummy,    NULL, NULL},
  {"bootflash_setchain", cmd_dummy,    NULL, NULL},
  {"cat",                cmd_dummy,    NULL, NULL},
  {"date",               cmd_dummy,    NULL, NULL},
  {"dev",                cmd_dummy,    NULL, NULL},
  {"help",               cmd_help,     NULL, "Show shell usage guide"},
  {"i2c_scan",           cmd_bad_args, "<bus>", "Scan i2c bus"},
  {"ls",                 cmd_dummy,    NULL, NULL},
  {"mem",                cmd_dummy,    NULL, NULL},
  {"metric",             cmd_dummy,    NULL, NULL},
  {"net_ipv4_show",      cmd_dummy,    NULL, NULL},
  {"net_ipv4_status",    cmd_dummy,    NULL, NULL},
  {"net_ble_scan",       cmd_dummy,    NULL, NULL},
  {"ps",                 cmd_dummy,    NULL, "Show running tasks"},
  {"reset",              cmd_bad_args, NULL, "Reset the system"},
  {"show_devices",       cmd_dummy,    NULL, NULL},
  {"show_gpio",          cmd_dummy,    NULL, NULL},
  {"show_tasks",         cmd_dummy,    NULL, NULL},
  {"sysinfo",            cmd_dummy,    NULL, NULL},
  {"uptime",             cmd_dummy,    NULL, "Show system uptime"},
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
static int hist_wpos;
static int hist_count;

static void
history_reset(void)
{
  for(int i = 0; i < HIST_MAX; i++) {
    free(hist_lines[i]);
    hist_lines[i] = NULL;
  }
  hist_wpos = 0;
  hist_count = 0;
}

static void
history_add(const char *line)
{
  if(hist_count > 0) {
    int last = (hist_wpos - 1 + HIST_MAX) % HIST_MAX;
    if(!strcmp(line, hist_lines[last]))
      return;
  }
  free(hist_lines[hist_wpos]);
  hist_lines[hist_wpos] = strdup(line);
  hist_wpos = (hist_wpos + 1) % HIST_MAX;
  hist_count++;
}

static char *
history_get(int offset)
{
  int avail = hist_count < HIST_MAX ? hist_count : HIST_MAX;
  if(offset < 0 || offset >= avail)
    return NULL;
  int idx = (hist_wpos - 1 - offset + HIST_MAX) % HIST_MAX;
  return strdup(hist_lines[idx]);
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

static error_t cmd_help(cli_t *c, int argc, char **argv);
CLI_CMD_DEF_EXT("help", cmd_help, NULL, "Show shell usage guide")

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


#ifdef ENABLE_CLI_EXTENDED_HELP
static void
print_cmd_name(cli_t *c, const char *cmd)
{
  for(const char *p = cmd; *p; p++)
    cli_printf(c, "%c", *p == '_' ? ' ' : *p);
}
#endif

static void
dispatch_command(cli_t *c, char *line)
{
  char *argv[CLI_MAX_ARGC];
  int argc = tokenize(line, argv, CLI_MAX_ARGC);
  if(argc == 0)
    return;

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
        if(err) {
#ifdef ENABLE_CLI_EXTENDED_HELP
          if(err == ERR_INVALID_ARGS && p->synopsis) {
            cli_printf(c, "Usage: ");
            print_cmd_name(c, p->cmd);
            cli_printf(c, " %s\n", p->synopsis);
          } else
#endif
            cli_printf(c, "! Error: %s\n", error_to_string(err));
        }
        return;
      }
    }
  }

  // Check if it could be a valid group prefix
  for(const cli_cmd_t *p = begin; p != end; p++) {
    const char *r = match_argv(p->cmd, argv, argc);
    if(r && *r == '_') {
      cli_printf(c, "Incomplete command. Press ? for options.\n");
      return;
    }
  }

  cli_printf(c, "Unknown command\n");
}


static error_t
cmd_help(cli_t *c, int argc, char **argv)
{
  (void)argc;
  (void)argv;
  cli_printf(c,
    "  ?     List commands (context-sensitive)\n"
    "  TAB   Complete command\n"
    "  Emacs-style line editing (Ctrl-A, Ctrl-E, Ctrl-W, ...)\n"
  );
  return 0;
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

#ifdef ENABLE_CLI_EXTENDED_HELP
  // "i2c scan ?" — trailing space means plen overshoots the command name.
  // Strip it and check for an exact match with extended help.
  if(list_only && nmatch == 0 && plen > 0 && ed->buf[plen - 1] == ' ') {
    int trimmed = plen - 1;
    for(const cli_cmd_t *p = begin; p != end; p++) {
      if((int)strlen(p->cmd) == trimmed &&
         prefix_match(p->cmd, ed->buf, trimmed)) {
        cli_printf(c, "\n");
        if(p->synopsis) {
          print_cmd_name(c, p->cmd);
          cli_printf(c, " %s\n", p->synopsis);
        } else if(p->brief) {
          print_cmd_name(c, p->cmd);
          cli_printf(c, " - %s\n", p->brief);
        }
        return;
      }
    }
  }
#endif

  if(nmatch == 0)
    return;

  if(!list_only) {
    if(common > plen) {
      for(int i = plen; i < common; i++) {
        char ch = (first->cmd[i] == '_') ? ' ' : first->cmd[i];
        cli_ed_insert(ed, &ch, 1);
      }

      // Add trailing space if we completed to a full command or group boundary
      if(nmatch == 1 && first->cmd[common] == '\0') {
        cli_ed_insert(ed, " ", 1);
      } else if(nmatch > 0 && first->cmd[common] == '_') {
        cli_ed_insert(ed, " ", 1);
      }
      return;
    }
  }

  // Can't extend further — list alternatives (pass 2)
  cli_printf(c, "\n");

#ifdef ENABLE_CLI_EXTENDED_HELP
  if(list_only && nmatch == 1 && (int)strlen(first->cmd) == plen) {
    if(first->synopsis) {
      print_cmd_name(c, first->cmd);
      cli_printf(c, " %s\n", first->synopsis);
    } else if(first->brief) {
      print_cmd_name(c, first->cmd);
      cli_printf(c, " - %s\n", first->brief);
    }
    return;
  }
#endif

  // Find word boundary: position after last space in editor buffer
  int word_start = plen;
  while(word_start > 0 && ed->buf[word_start - 1] != ' ')
    word_start--;

  const char *prev_seg = NULL;
  int prev_len = 0;

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

#ifdef ENABLE_CLI_EXTENDED_HELP
    if(*seg_end == '\0' && p->brief)
      cli_printf(c, "  %-16.*s %s\n", seg_len, suffix, p->brief);
    else
#endif
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
  int hist_offset = -1;
  char *saved_line = NULL;

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
      hist_offset = -1;
      free(saved_line);
      saved_line = NULL;
      redraw_line(c, &ed, promptchar);
      break;

    case CLI_ED_TAB:
      do_completion(c, &ed, 0);
      redraw_line(c, &ed, promptchar);
      break;

    case CLI_ED_HELP:
      do_completion(c, &ed, 1);
      redraw_line(c, &ed, promptchar);
      break;

    case CLI_ED_HIST_UP: {
      int next = hist_offset + 1;
      char *str = history_get(next);
      if(str) {
        if(hist_offset == -1)
          saved_line = strdup(ed.buf);
        hist_offset = next;
        cli_ed_set(&ed, str);
        free(str);
      }
      redraw_line(c, &ed, promptchar);
      break;
    }

    case CLI_ED_HIST_DOWN: {
      if(hist_offset > 0) {
        hist_offset--;
        char *str = history_get(hist_offset);
        cli_ed_set(&ed, str);
        free(str);
      } else if(hist_offset == 0) {
        hist_offset = -1;
        cli_ed_set(&ed, saved_line);
        free(saved_line);
        saved_line = NULL;
      }
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
      hist_offset = -1;
      free(saved_line);
      saved_line = NULL;
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
static int feed_hist_offset = -1;
static char *feed_saved_line;

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
      feed_hist_offset = -1;
      free(feed_saved_line);
      feed_saved_line = NULL;
      break;
    case CLI_ED_HIST_UP: {
      int next = feed_hist_offset + 1;
      char *str = history_get(next);
      if(str) {
        if(feed_hist_offset == -1)
          feed_saved_line = strdup(ed->buf);
        feed_hist_offset = next;
        cli_ed_set(ed, str);
        free(str);
      }
      break;
    }
    case CLI_ED_HIST_DOWN: {
      if(feed_hist_offset > 0) {
        feed_hist_offset--;
        char *str = history_get(feed_hist_offset);
        cli_ed_set(ed, str);
        free(str);
      } else if(feed_hist_offset == 0) {
        feed_hist_offset = -1;
        cli_ed_set(ed, feed_saved_line);
        free(feed_saved_line);
        feed_saved_line = NULL;
      }
      break;
    }
    case CLI_ED_CANCEL:
      cli_ed_clear(ed);
      feed_hist_offset = -1;
      free(feed_saved_line);
      feed_saved_line = NULL;
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
  feed_hist_offset = -1;
  free(feed_saved_line);
  feed_saved_line = NULL;
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
  CHECK(test_output_contains("?"));
  CHECK(test_output_contains("TAB"));
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
// History tests
// ====================================================================

static void
test_history_up_most_recent(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  feed_input(&c, &ed, "arp\r");
  feed_input(&c, &ed, "date\r");
  feed_input(&c, &ed, "mem\r");
  // UP should recall most recent
  feed_input(&c, &ed, "\x1b[A");
  CHECK(!strcmp(ed.buf, "mem"));
}

static void
test_history_up_up_older(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  feed_input(&c, &ed, "arp\r");
  feed_input(&c, &ed, "date\r");
  feed_input(&c, &ed, "mem\r");
  // UP UP should recall second most recent
  feed_input(&c, &ed, "\x1b[A\x1b[A");
  CHECK(!strcmp(ed.buf, "date"));
}

static void
test_history_up_down_restore(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  feed_input(&c, &ed, "arp\r");
  feed_input(&c, &ed, "date\r");
  feed_input(&c, &ed, "mem\r");
  // Type partial, UP, then DOWN should restore partial
  feed_input(&c, &ed, "part");
  feed_input(&c, &ed, "\x1b[A");
  CHECK(!strcmp(ed.buf, "mem"));
  feed_input(&c, &ed, "\x1b[B");
  CHECK(!strcmp(ed.buf, "part"));
}

static void
test_history_down_from_fresh(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  feed_input(&c, &ed, "arp\r");
  // DOWN from fresh prompt — no crash, buffer empty
  feed_input(&c, &ed, "\x1b[B");
  CHECK(!strcmp(ed.buf, ""));
}

static void
test_history_up_at_oldest(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  feed_input(&c, &ed, "arp\r");
  feed_input(&c, &ed, "date\r");
  // UP UP UP — third UP stays at oldest
  feed_input(&c, &ed, "\x1b[A\x1b[A\x1b[A");
  CHECK(!strcmp(ed.buf, "arp"));
}

static void
test_history_duplicate_suppression(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  feed_input(&c, &ed, "arp\r");
  feed_input(&c, &ed, "arp\r");
  // UP should give "arp", second UP should have nothing
  feed_input(&c, &ed, "\x1b[A");
  CHECK(!strcmp(ed.buf, "arp"));
  feed_input(&c, &ed, "\x1b[A");
  CHECK(!strcmp(ed.buf, "arp")); // stays at oldest
}

static void
test_history_overflow(void)
{
  cli_t c;
  cli_ed_t ed;
  test_reset(&c, &ed);

  // Add 10 entries, only last 8 should be accessible
  char buf[8];
  for(int i = 0; i < 10; i++) {
    snprintf(buf, sizeof(buf), "cmd%d", i);
    history_add(buf);
  }
  // Most recent is cmd9, oldest accessible is cmd2
  char *str = history_get(0);
  CHECK(str && !strcmp(str, "cmd9"));
  free(str);
  str = history_get(7);
  CHECK(str && !strcmp(str, "cmd2"));
  free(str);
  str = history_get(8);
  CHECK(str == NULL);
}


// ====================================================================
// Extended help tests
// ====================================================================

static void
test_help_question_synopsis(void)
{
  // '?' on exact command with synopsis should show usage
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);
  test_output_reset();

  cli_ed_set(&ed, "i2c scan");
  do_completion(&c, &ed, 1);
  CHECK(test_output_contains("i2c scan <bus>"));
}

static void
test_help_question_brief_only(void)
{
  // '?' on exact command with brief but no synopsis
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);
  test_output_reset();

  cli_ed_set(&ed, "reset");
  do_completion(&c, &ed, 1);
  CHECK(test_output_contains("reset - Reset the system"));
}

static void
test_help_question_no_extended(void)
{
  // '?' on exact command with neither synopsis nor brief
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);
  test_output_reset();

  cli_ed_set(&ed, "arp");
  do_completion(&c, &ed, 1);
  // Should not crash; output should be minimal (just newline from pass 2)
  CHECK(!test_output_contains("arp -"));
  CHECK(!test_output_contains("arp <"));
}

static void
test_help_question_trailing_space(void)
{
  // '?' after "i2c scan " (trailing space) should still show synopsis
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);
  test_output_reset();

  cli_ed_set(&ed, "i2c scan ");
  do_completion(&c, &ed, 1);
  CHECK(test_output_contains("i2c scan <bus>"));
}

static void
test_dispatch_invalid_args_with_synopsis(void)
{
  // ERR_INVALID_ARGS with synopsis should show usage
  cli_t c = {};
  dispatch_tracking_reset();
  test_output_reset();
  char line[] = "i2c scan";
  dispatch_command(&c, line);
  CHECK(!strcmp(last_dispatched_cmd, "i2c_scan"));
  CHECK(test_output_contains("Usage: i2c scan <bus>"));
  CHECK(!test_output_contains("! Error:"));
}

static void
test_dispatch_invalid_args_without_synopsis(void)
{
  // ERR_INVALID_ARGS without synopsis should show old error format
  cli_t c = {};
  dispatch_tracking_reset();
  test_output_reset();
  char line[] = "reset";
  dispatch_command(&c, line);
  CHECK(!strcmp(last_dispatched_cmd, "reset"));
  CHECK(test_output_contains("! Error:"));
  CHECK(!test_output_contains("Usage:"));
}

static void
test_help_brief_column(void)
{
  // ? listing should show brief text for leaf commands
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);
  test_output_reset();

  cli_ed_set(&ed, "");
  do_completion(&c, &ed, 1);
  CHECK(test_output_contains("Reset the system"));
  CHECK(test_output_contains("Show system uptime"));
}

static void
test_help_brief_not_for_groups(void)
{
  // ? listing: groups should not show brief text (only leaf commands do)
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);
  test_output_reset();

  cli_ed_set(&ed, "");
  do_completion(&c, &ed, 1);
  // "i2c" is a group prefix at top level, should not show "Scan i2c bus"
  CHECK(!test_output_contains("Scan i2c bus"));
}

static void
test_completion_listing_brief(void)
{
  // Listing alternatives should show brief for leaf commands
  cli_t c = {};
  cli_ed_t ed;
  cli_ed_init(&ed);
  test_output_reset();

  // "i2c " with list_only=1 (?) lists subcommands with brief
  cli_ed_set(&ed, "i2c ");
  do_completion(&c, &ed, 1);
  CHECK(test_output_contains("Scan i2c bus"));
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

  // Dispatch tests
  {"dispatch: simple command",       test_dispatch_simple},
  {"dispatch: grouped command",      test_dispatch_grouped},
  {"dispatch: grouped with args",    test_dispatch_grouped_with_args},
  {"dispatch: multi-level",          test_dispatch_multilevel},
  {"dispatch: multi-level with args",test_dispatch_multilevel_with_args},
  {"dispatch: unknown command",      test_dispatch_unknown},
  {"dispatch: incomplete group",     test_dispatch_incomplete_group},

  // Extended help tests
  {"exthelp: ? synopsis",              test_help_question_synopsis},
  {"exthelp: ? brief only",            test_help_question_brief_only},
  {"exthelp: ? no extended",           test_help_question_no_extended},
  {"exthelp: ? trailing space",        test_help_question_trailing_space},
  {"exthelp: ERR_INVALID_ARGS+synopsis", test_dispatch_invalid_args_with_synopsis},
  {"exthelp: ERR_INVALID_ARGS no synopsis", test_dispatch_invalid_args_without_synopsis},
  {"exthelp: help brief column",       test_help_brief_column},
  {"exthelp: brief not for groups",    test_help_brief_not_for_groups},
  {"exthelp: completion listing brief", test_completion_listing_brief},

  // History tests
  {"history: UP recalls most recent",   test_history_up_most_recent},
  {"history: UP UP recalls older",      test_history_up_up_older},
  {"history: UP then DOWN restores",    test_history_up_down_restore},
  {"history: DOWN from fresh prompt",   test_history_down_from_fresh},
  {"history: UP at oldest stays",       test_history_up_at_oldest},
  {"history: duplicate suppression",    test_history_duplicate_suppression},
  {"history: overflow",                 test_history_overflow},

  // Integration tests
  {"integ: tab complete and execute",  test_integration_tab_then_execute},
  {"integ: help via enter",            test_integration_help_via_enter},
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
