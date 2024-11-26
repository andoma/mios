#pragma once

#include "task.h"

#define HOSTNAME_BUFFER_SIZE 32

extern char hostname[HOSTNAME_BUFFER_SIZE];

extern mutex_t hostname_mutex;

void hostname_set(const char *name);

void hostname_setf(const char *fmt, ...);
