#include <mios/stream.h>
#include <stdlib.h>

#include "hdlc.h"
#include "crc32.h"

void
hdlc_read(stream_t *s,
          void (*cb)(void *opaque, const uint8_t *data, size_t len),
          void *opaque, size_t max_frame_size)
{
  int len = -1;
  uint8_t *buf;
  if(max_frame_size <= 16)
    buf = __builtin_alloca(max_frame_size); // FIXME: Add alloca.h
  else
    buf = malloc(max_frame_size);

  while(1) {

    char c;
    s->read(s, &c, 1, STREAM_READ_WAIT_ALL);

    switch(c) {
    case 0x7e:
      if(len > 4 && (uint32_t)~crc32(0, buf, len) == 0)
        cb(opaque, buf, len - 4);
      len = 0;
      break;
    case 0x7d:
      s->read(s, &c, 1, STREAM_READ_WAIT_ALL);
      c ^= 0x20;
    default:
      if(len >= 0) {
        buf[len] = c;
        len++;
        if(len > max_frame_size) {
          len = -1;
        }
      }
      break;
    }
  }
}



void
hdlc_write_rawv(stream_t *s, struct iovec *iov, size_t count)
{
  const uint8_t byte_7e = 0x7e;
  uint8_t esc[2] = {0x7d, 0};

  s->write(s, &byte_7e, 1);

  for(size_t v = 0; v < count; v++) {

    const uint8_t *buf = iov[v].iov_base;
    const size_t len = iov[v].iov_len;
    size_t i, b = 0;
    for(i = 0; i < len; i++) {
      if(buf[i] == 0x7e || buf[i] == 0x7d) {

        if(i - b)
          s->write(s, buf + b, i - b);

        esc[1] = buf[i] ^ 0x20;
        s->write(s, esc, 2);

        b = i + 1;
      }
    }
    if(i - b)
      s->write(s, buf + b, i - b);
  }
  s->write(s, &byte_7e, 1);
}


void
hdlc_send(stream_t *s, const void *data, size_t len)
{
  uint32_t crc = ~crc32(0, data, len);

  struct iovec vec[2] = {
    { .iov_base = (void *)data,
      .iov_len = len
    }, {
      .iov_base = &crc,
      .iov_len = 4
    }
  };

  hdlc_write_rawv(s, vec, 2);
}
