#pragma once

#include <mios/mios.h>
#include <mios/stream.h>
#include <mios/error.h>

typedef struct copy_handler {
  const char *prefix;
  stream_t *(*open_write)(const char *url);
  error_t (*read_to)(const char *url, stream_t *output);
} copy_handler_t;

#define COPY_HANDLER_DEF(name, prio, ...)                               \
  static const copy_handler_t MIOS_JOIN(copyhandler, __LINE__)          \
  __attribute__((used, section("copyhandler." #prio))) = { __VA_ARGS__ }
