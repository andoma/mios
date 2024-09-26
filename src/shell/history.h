#pragma once

void history_add(const char *line);
void history_up();
void history_down();
/* The called must free the returned pointer if not NULL */
char *history_get_current_line(void);
