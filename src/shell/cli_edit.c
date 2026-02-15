#include <string.h>
#include <stdio.h>
#include "cli_edit.h"

void
cli_ed_init(cli_ed_t *ed)
{
  memset(ed, 0, sizeof(*ed));
}

void
cli_ed_insert(cli_ed_t *ed, const char *text, int tlen)
{
  for(int i = 0; i < tlen; i++) {
    if(ed->len >= CLI_LINE_BUF_SIZE - 1)
      break;
    memmove(ed->buf + ed->pos + 1, ed->buf + ed->pos, ed->len - ed->pos);
    ed->buf[ed->pos] = text[i];
    ed->pos++;
    ed->len++;
    ed->buf[ed->len] = '\0';
  }
}

void
cli_ed_set(cli_ed_t *ed, const char *text)
{
  if(text) {
    int tlen = strlen(text);
    if(tlen >= CLI_LINE_BUF_SIZE)
      tlen = CLI_LINE_BUF_SIZE - 1;
    memcpy(ed->buf, text, tlen);
    ed->buf[tlen] = '\0';
    ed->pos = tlen;
    ed->len = tlen;
  } else {
    ed->buf[0] = '\0';
    ed->pos = 0;
    ed->len = 0;
  }
}

void
cli_ed_clear(cli_ed_t *ed)
{
  ed->buf[0] = '\0';
  ed->pos = 0;
  ed->len = 0;
}

static void
delete_char_at(cli_ed_t *ed, int pos)
{
  if(pos >= ed->len)
    return;
  memmove(ed->buf + pos, ed->buf + pos + 1, ed->len - pos - 1);
  ed->len--;
  ed->buf[ed->len] = '\0';
}

static void
kill_word_back(cli_ed_t *ed)
{
  if(ed->pos == 0)
    return;

  int end = ed->pos;

  // Skip trailing spaces
  while(ed->pos > 0 && ed->buf[ed->pos - 1] == ' ')
    ed->pos--;

  // Skip word characters
  while(ed->pos > 0 && ed->buf[ed->pos - 1] != ' ')
    ed->pos--;

  int removed = end - ed->pos;
  memmove(ed->buf + ed->pos, ed->buf + end, ed->len - end);
  ed->len -= removed;
  ed->buf[ed->len] = '\0';
}

static void
word_forward(cli_ed_t *ed)
{
  // Skip current word
  while(ed->pos < ed->len && ed->buf[ed->pos] != ' ')
    ed->pos++;
  // Skip spaces
  while(ed->pos < ed->len && ed->buf[ed->pos] == ' ')
    ed->pos++;
}

static void
word_backward(cli_ed_t *ed)
{
  // Skip spaces
  while(ed->pos > 0 && ed->buf[ed->pos - 1] == ' ')
    ed->pos--;
  // Skip word
  while(ed->pos > 0 && ed->buf[ed->pos - 1] != ' ')
    ed->pos--;
}

static cli_ed_event_t
handle_csi_final(cli_ed_t *ed, int final)
{
  int p1 = ed->esc_param1;
  int p2 = ed->esc_param2;

  switch(final) {
  case 'A': // Up
    return CLI_ED_HIST_UP;
  case 'B': // Down
    return CLI_ED_HIST_DOWN;
  case 'C': // Right
    if(p2 == 5) {
      // Ctrl-Right: word forward
      word_forward(ed);
    } else {
      if(ed->pos < ed->len)
        ed->pos++;
    }
    return CLI_ED_NONE;
  case 'D': // Left
    if(p2 == 5) {
      // Ctrl-Left: word backward
      word_backward(ed);
    } else {
      if(ed->pos > 0)
        ed->pos--;
    }
    return CLI_ED_NONE;
  case 'H': // Home
    ed->pos = 0;
    return CLI_ED_NONE;
  case 'F': // End
    ed->pos = ed->len;
    return CLI_ED_NONE;
  case '~':
    if(p1 == 3) {
      // Delete key
      delete_char_at(ed, ed->pos);
    }
    return CLI_ED_NONE;
  default:
    return CLI_ED_NONE;
  }
}

cli_ed_event_t
cli_ed_input(cli_ed_t *ed, int c)
{
  switch(ed->esc_state) {

  case CLI_ESC_ESC:
    ed->esc_state = CLI_ESC_NONE;
    if(c == '[') {
      ed->esc_state = CLI_ESC_BRACKET;
      ed->esc_param1 = 0;
      ed->esc_param2 = 0;
      return CLI_ED_NONE;
    }
    if(c == 'b') {
      word_backward(ed);
      return CLI_ED_NONE;
    }
    if(c == 'f') {
      word_forward(ed);
      return CLI_ED_NONE;
    }
    // Unknown escape, ignore
    return CLI_ED_NONE;

  case CLI_ESC_BRACKET:
    if(c >= '0' && c <= '9') {
      ed->esc_param1 = c - '0';
      ed->esc_state = CLI_ESC_PARAM1;
      return CLI_ED_NONE;
    }
    ed->esc_state = CLI_ESC_NONE;
    return handle_csi_final(ed, c);

  case CLI_ESC_PARAM1:
    if(c >= '0' && c <= '9') {
      ed->esc_param1 = ed->esc_param1 * 10 + (c - '0');
      return CLI_ED_NONE;
    }
    if(c == ';') {
      ed->esc_state = CLI_ESC_SEMI;
      return CLI_ED_NONE;
    }
    ed->esc_state = CLI_ESC_NONE;
    return handle_csi_final(ed, c);

  case CLI_ESC_SEMI:
    if(c >= '0' && c <= '9') {
      ed->esc_param2 = c - '0';
      ed->esc_state = CLI_ESC_PARAM2;
      return CLI_ED_NONE;
    }
    ed->esc_state = CLI_ESC_NONE;
    return CLI_ED_NONE;

  case CLI_ESC_PARAM2:
    if(c >= '0' && c <= '9') {
      ed->esc_param2 = ed->esc_param2 * 10 + (c - '0');
      return CLI_ED_NONE;
    }
    ed->esc_state = CLI_ESC_NONE;
    return handle_csi_final(ed, c);

  default:
    break;
  }

  // Normal input
  switch(c) {
  case 27: // ESC
    ed->esc_state = CLI_ESC_ESC;
    return CLI_ED_NONE;

  case 1: // Ctrl-A
    ed->pos = 0;
    return CLI_ED_NONE;

  case 2: // Ctrl-B
    if(ed->pos > 0)
      ed->pos--;
    return CLI_ED_NONE;

  case 3: // Ctrl-C
    return CLI_ED_CANCEL;

  case 4: // Ctrl-D
    if(ed->len == 0)
      return CLI_ED_EOF;
    delete_char_at(ed, ed->pos);
    return CLI_ED_NONE;

  case 5: // Ctrl-E
    ed->pos = ed->len;
    return CLI_ED_NONE;

  case 6: // Ctrl-F
    if(ed->pos < ed->len)
      ed->pos++;
    return CLI_ED_NONE;

  case 9: // Tab
    return CLI_ED_TAB;

  case 10: // LF
  case 13: // CR
    return CLI_ED_ENTER;

  case 11: // Ctrl-K: kill to end of line
    ed->len = ed->pos;
    ed->buf[ed->len] = '\0';
    return CLI_ED_NONE;

  case 12: // Ctrl-L
    return CLI_ED_CLEAR;

  case 21: // Ctrl-U: kill entire line
    cli_ed_clear(ed);
    return CLI_ED_NONE;

  case 23: // Ctrl-W: kill previous word
    kill_word_back(ed);
    return CLI_ED_NONE;

  case 127: // Backspace
    if(ed->pos > 0) {
      ed->pos--;
      delete_char_at(ed, ed->pos);
    }
    return CLI_ED_NONE;

  case 8: // Ctrl-H (some terminals send this for backspace)
    if(ed->pos > 0) {
      ed->pos--;
      delete_char_at(ed, ed->pos);
    }
    return CLI_ED_NONE;

  case '?':
    return CLI_ED_HELP;

  default:
    if(c >= 32 && c < 127) {
      if(ed->len < CLI_LINE_BUF_SIZE - 1) {
        memmove(ed->buf + ed->pos + 1, ed->buf + ed->pos, ed->len - ed->pos);
        ed->buf[ed->pos] = c;
        ed->pos++;
        ed->len++;
        ed->buf[ed->len] = '\0';
      }
    }
    return CLI_ED_NONE;
  }
}
