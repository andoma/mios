#pragma once

#ifndef CLI_LINE_BUF_SIZE
#define CLI_LINE_BUF_SIZE 80
#endif

#include <stdint.h>

typedef enum {
  CLI_ED_NONE,
  CLI_ED_ENTER,
  CLI_ED_TAB,
  CLI_ED_HELP,
  CLI_ED_HIST_UP,
  CLI_ED_HIST_DOWN,
  CLI_ED_CLEAR,
  CLI_ED_CANCEL,
  CLI_ED_EOF,
} cli_ed_event_t;

enum {
  CLI_ESC_NONE,
  CLI_ESC_ESC,
  CLI_ESC_BRACKET,
  CLI_ESC_PARAM1,
  CLI_ESC_SEMI,
  CLI_ESC_PARAM2,
  CLI_ESC_TILDE,
};

typedef struct cli_ed {
  char buf[CLI_LINE_BUF_SIZE];
  int16_t pos;
  int16_t len;
  uint8_t esc_state;
  uint8_t esc_param1;
  uint8_t esc_param2;
} cli_ed_t;

void           cli_ed_init(cli_ed_t *ed);
cli_ed_event_t cli_ed_input(cli_ed_t *ed, int c);
void           cli_ed_insert(cli_ed_t *ed, const char *text, int len);
void           cli_ed_set(cli_ed_t *ed, const char *text);
void           cli_ed_clear(cli_ed_t *ed);
