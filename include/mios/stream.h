#pragma once

typedef struct stream {
  int (*read)(struct stream *s, void *buf, size_t size, int wait);
  void (*write)(struct stream *s, const void *buf, size_t size);
} stream_t;
