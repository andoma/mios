#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_LINES 8

static struct history {
  char *lines[MAX_LINES];
  char **latest;
  char **current;
} history;

void history_init(void)
{
  memset(history.lines, 0, sizeof(char *) * MAX_LINES);
  history.current = NULL;
  history.latest = NULL;
}

static char *zstrdup(const char *line)
{
  size_t len = strlen(line);
  char *new = calloc(len + 1, 1);
  strlcpy(new, line, len + 1);
  return new;
}

void history_add(const char *line)
{
  if (history.latest == NULL) {
    history.lines[0] = zstrdup(line);
    history.latest = history.lines;
  } else {
    history.latest++;
    if (history.latest >= &history.lines[MAX_LINES])
      history.latest = history.lines;
    if (*history.latest)
      free(*history.latest);
    *history.latest = zstrdup(line);
  }
  history.current = history.latest;
}

void history_up()
{
  if (history.current == NULL)
    return;
  history.current--;
  if (history.current < history.lines) {
    history.current = &history.lines[MAX_LINES - 1];
    while (*history.current == NULL)
      history.current--;
  }
}

void history_down()
{
  if (history.current == NULL)
    return;
  history.current++;
  if (history.current >= &history.lines[MAX_LINES]) {
    history.current = history.lines;
    while (*history.current == NULL)
      history.current++;
    history.current = history.lines;
  }
}

char *history_get_current_line(void)
{
  if (history.current == NULL)
    return NULL;
  return *history.current;
}
