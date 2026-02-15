#pragma once

void history_add(const char *line);

/* Returns strdup'd copy. offset 0 = most recent. Returns NULL if out of range.
   Caller must free. */
char *history_get(int offset);
