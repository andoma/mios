#pragma once

void history_init(void);
void history_add(const char *line);
void history_up();
void history_down();
char *history_get_current_line(void);
