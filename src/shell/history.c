#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include <mios/task.h>

#define MAX_LINES 8

static mutex_t history_mutex = MUTEX_INITIALIZER("history");

static struct history {
  char *lines[MAX_LINES];
  char **latest;
  char **current;
} history;

void
history_add(const char *line)
{
  mutex_lock(&history_mutex);
  if (history.latest == NULL) {
    history.lines[0] = strdup(line);
    history.latest = history.lines;
  } else if (strcmp(line, *history.latest)) {
    history.latest++;
    if (history.latest >= &history.lines[MAX_LINES])
      history.latest = history.lines;
    if (*history.latest)
      free(*history.latest);
    *history.latest = strdup(line);
  }
  history.current = history.latest;
  mutex_unlock(&history_mutex);
}

void
history_up(void)
{
  mutex_lock(&history_mutex);
  if (history.current == NULL) {
    mutex_unlock(&history_mutex);
    return;
  }
  history.current--;
  if (history.current < history.lines) {
    history.current = &history.lines[MAX_LINES - 1];
    while (*history.current == NULL)
      history.current--;
  }
  mutex_unlock(&history_mutex);
}

void
history_down(void)
{
  mutex_lock(&history_mutex);
  if (history.current == NULL) {
    mutex_unlock(&history_mutex);
    return;
  }
  history.current++;
  if (history.current >= &history.lines[MAX_LINES]) {
    history.current = history.lines;
    while (*history.current == NULL)
      history.current++;
    history.current = history.lines;
  }
  mutex_unlock(&history_mutex);
}

char *
history_get_current_line(void)
{
  char *ret;
  mutex_lock(&history_mutex);
  if (history.current == NULL) {
      mutex_unlock(&history_mutex);
      return NULL;
  }
  ret = strdup(*history.current);
  mutex_unlock(&history_mutex);
  return ret;
}
