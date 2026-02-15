#include <string.h>
#include <stdlib.h>
#include <mios/task.h>

#define HISTORY_SIZE 8

static mutex_t history_mutex = MUTEX_INITIALIZER("history");

static char *history_lines[HISTORY_SIZE];
static int history_wpos;    // next write position (circular)
static int history_count;   // total entries ever added

void
history_add(const char *line)
{
  mutex_lock(&history_mutex);

  if(history_count > 0) {
    int last = (history_wpos - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    if(!strcmp(line, history_lines[last])) {
      mutex_unlock(&history_mutex);
      return;
    }
  }

  free(history_lines[history_wpos]);
  history_lines[history_wpos] = strdup(line);
  history_wpos = (history_wpos + 1) % HISTORY_SIZE;
  history_count++;

  mutex_unlock(&history_mutex);
}

char *
history_get(int offset)
{
  char *ret = NULL;
  mutex_lock(&history_mutex);

  int avail = history_count < HISTORY_SIZE ? history_count : HISTORY_SIZE;
  if(offset >= 0 && offset < avail) {
    int idx = (history_wpos - 1 - offset + HISTORY_SIZE) % HISTORY_SIZE;
    ret = strdup(history_lines[idx]);
  }

  mutex_unlock(&history_mutex);
  return ret;
}
