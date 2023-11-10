#include <stdio.h>

#include <mios/stream.h>
#include <stdlib.h>
#include <alloca.h>

#include "hdlc.h"

#ifdef __mios__
#include "crc32.h"
#else
#include <zlib.h>
#endif

int
hdlc_read_to_buf(stream_t *s, uint8_t *buf, size_t max_frame_size, int wait)
{
  int len = -1;

  while(1) {

    char c;
    if(s->read(s, &c, 1, len < 1 ? wait : STREAM_READ_WAIT_ALL) == 0)
      return 0;

    switch(c) {
    case 0x7e:
      if(len > 4 && (uint32_t)~crc32(0, buf, len) == 0)
        return len - 4;
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
hdlc_read(stream_t *s,
          void (*cb)(void *opaque, uint8_t *data, size_t len),
          void *opaque, size_t max_frame_size)
{
  uint8_t *buf;
  buf = max_frame_size <= 16 ? alloca(max_frame_size) : malloc(max_frame_size);

  while(1) {
    cb(opaque, buf,
       hdlc_read_to_buf(s, buf, max_frame_size, STREAM_READ_WAIT_ALL));
  }
}



void
hdlc_write_rawv(stream_t *s, struct iovec *iov, size_t count)
{
  const uint8_t byte_7e = 0x7e;
  uint8_t esc[2] = {0x7d, 0};

  s->write(s, &byte_7e, 1, 0);

  for(size_t v = 0; v < count; v++) {

    const uint8_t *buf = iov[v].iov_base;
    const size_t len = iov[v].iov_len;
    size_t i, b = 0;
    for(i = 0; i < len; i++) {
      if(buf[i] == 0x7e || buf[i] == 0x7d) {

        if(i - b)
          s->write(s, buf + b, i - b, 0);

        esc[1] = buf[i] ^ 0x20;
        s->write(s, esc, 2, 0);

        b = i + 1;
      }
    }
    if(i - b)
      s->write(s, buf + b, i - b, 0);
  }
  s->write(s, &byte_7e, 1, 0);
  s->write(s, NULL, 0, 0);
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

void
hdlc_sendv(stream_t *s, struct iovec *iov, size_t count)
{
  uint32_t crc = 0;
  struct iovec vec[count + 1];
  for(size_t i = 0; i < count; i++) {
    crc = crc32(crc, iov[i].iov_base, iov[i].iov_len);
    vec[i].iov_base = iov[i].iov_base;
    vec[i].iov_len = iov[i].iov_len;
  }

  crc = ~crc;
  vec[count].iov_base = &crc;
  vec[count].iov_len = 4;
  hdlc_write_rawv(s, vec, count + 1);
}
