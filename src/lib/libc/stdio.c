#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <mios/stream.h>

#ifdef ENABLE_NET
#include <net/net.h>
#endif

extern va_list fmt_double(va_list ap, char *buf, size_t buflen);

va_list  __attribute__((weak))
fmt_double(va_list ap, char *buf, size_t buflen)
{
  strlcpy(buf, "<nomath>", buflen);
  return ap;
}

typedef struct {
  int16_t width;
  unsigned char lz:1;
  unsigned char la:1;
#ifdef ENABLE_NET
  unsigned char ipv4:1;
#endif
} fmtparam_t;


static size_t
emit_repeated_char(fmtcb_t *cb, void *aux, ssize_t len, char c)
{
  if(len < 0)
    return 0;
  for(int i = 0; i < len; i++) {
    cb(aux, &c, 1);
  }
  return len;
}


static size_t __attribute__((noinline))
emit_str(fmtcb_t *cb, void *aux, const char *str,
         const fmtparam_t *fp)
{
  if(str == NULL)
    return cb(aux, "(null)", 6);

  size_t sl = strlen(str);
  size_t total = 0;
  const int pad = fp->width - sl;

  if(!fp->la)
    total += emit_repeated_char(cb, aux, pad, ' ');

  total += cb(aux, str, sl);

  if(fp->la)
    total += emit_repeated_char(cb, aux, pad, ' ');

  return total;
}


static size_t  __attribute__((noinline))
emit_u32(fmtcb_t *cb, void *aux, unsigned int x,
         const fmtparam_t *fp, int neg)
{
  char buf[10];
  int digits = 1;
  for(int i = 0; i < 10; i++, x /= 10) {
    const unsigned int d = x % 10;
    buf[9 - i] = '0' + d;
    if(d)
      digits = i + 1;
  }

  size_t total = 0;

  const int pad = fp->width - (digits + neg);
  if(!fp->la)
    total += emit_repeated_char(cb, aux, pad,
                                fp->lz ? '0' : ' ');

  if(neg)
    total += cb(aux, "-", 1);
  total += cb(aux, buf + sizeof(buf) - digits, digits);

  if(fp->la)
    total += emit_repeated_char(cb, aux, pad, ' ');

  return total;
}


static size_t  __attribute__((noinline))
emit_u64(fmtcb_t *cb, void *aux, uint64_t x,
         const fmtparam_t *fp, int neg)
{
  char buf[20];
  int digits = 1;
  for(int i = 0; i < 20; i++, x /= 10) {
    const unsigned int d = x % 10;
    buf[19 - i] = '0' + d;
    if(d)
      digits = i + 1;
  }

  size_t total = 0;

  const int pad = fp->width - (digits + neg);
  if(!fp->la)
    total += emit_repeated_char(cb, aux, pad,
                                fp->lz ? '0' : ' ');

  if(neg)
    total += cb(aux, "-", 1);
  total += cb(aux, buf + sizeof(buf) - digits, digits);

  if(fp->la)
    total += emit_repeated_char(cb, aux, pad, ' ');

  return total;
}


static size_t  __attribute__((noinline))
emit_s32(fmtcb_t *cb, void *aux, int x,
         const fmtparam_t *fp)
{
#ifdef ENABLE_NET
  if(fp->ipv4) {
    size_t r = 0;
    x = ntohl(x);
    char dot = '.';
    r += emit_u32(cb, aux, (x >> 24) & 0xff, fp, 0);
    r += cb(aux, &dot, 1);
    r += emit_u32(cb, aux, (x >> 16) & 0xff, fp, 0);
    r += cb(aux, &dot, 1);
    r += emit_u32(cb, aux, (x >> 8) & 0xff, fp, 0);
    r += cb(aux, &dot, 1);
    r += emit_u32(cb, aux, x & 0xff, fp, 0);
    return r;
  }
#endif
  if(x < 0)
    return emit_u32(cb, aux, -x, fp, 1);
  else
    return emit_u32(cb, aux, x, fp, 0);
}


static size_t  __attribute__((noinline))
emit_s64(fmtcb_t *cb, void *aux, int64_t x,
         const fmtparam_t *fp)
{
  if(x < 0)
    return emit_u64(cb, aux, -x, fp, 1);
  else
    return emit_u64(cb, aux, x, fp, 0);
}

static size_t  __attribute__((noinline))
emit_x32(fmtcb_t *cb, void *aux, unsigned int x,
         const fmtparam_t *fp)
{
  char buf[8];
  int digits = 1;
  for(int i = 0; i < 8; i++, x >>= 4) {
    const unsigned int d = x & 0xf;
    buf[7 - i] = "0123456789abcdef"[d];
    if(d)
      digits = i + 1;
  }

  size_t total = 0;

  if(fp->width > digits) {
    const int pad = fp->width - digits;
    for(int i = 0; i < pad; i++) {
      total += cb(aux, fp->lz ? "0" : " ", 1);
    }
  }
  return cb(aux, buf + 8 - digits, digits) + total;
}


static int  __attribute__((noinline))
parse_dec(const char **fmt, int defval)
{
  int c = **fmt;
  if(c < '0' || c > '9')
    return defval;

  int acc = 0;
  while(1) {
    acc *= 10;
    acc += c - '0';
    *fmt = *fmt + 1;
    c = **fmt;
    if(c < '0' || c > '9')
      return acc;
  }
}

size_t
fmtv(fmtcb_t *cb, void *aux, const char *fmt, va_list ap)
{
  const char *s = fmt;
  char c;
  size_t total = 0;
  char tmp[32];

  while((c = *fmt) != 0) {
    fmt++;
    if(c != '%')
      continue;

    if(s != fmt)
      total += cb(aux, s, fmt - s - 1);

    fmtparam_t fp;
    fp.lz = 0;
    fp.la = 0;
#ifdef ENABLE_NET
    fp.ipv4 = 0;
#endif
    if(*fmt == '0') {
      fp.lz = 1;
      fmt++;
    } else if(*fmt == '-') {
      fp.la = 1;
      fmt++;
    }

    fp.width = parse_dec(&fmt, -1);

#ifdef ENABLE_NET
    fp.ipv4 = fmt[0] == 'I';
    if(fp.ipv4)
      fmt++;
#endif
    const int ll = fmt[0] == 'l' && fmt[1] == 'l';
    if(ll)
      fmt += 2;

    c = *fmt++;
    switch(c) {
    case '%':
      total += cb(aux, "%", 1);
      break;
    case 'c':
      c = va_arg(ap, int);
      total += cb(aux, &c, 1);
      break;
    case 's':
      total += emit_str(cb, aux, va_arg(ap, const char *), &fp);
      break;
    case 'x':
      total += emit_x32(cb, aux, va_arg(ap, unsigned int), &fp);
      break;
    case 'd':
      if(ll)
        total += emit_s64(cb, aux, va_arg(ap, uint64_t), &fp);
      else
        total += emit_s32(cb, aux, va_arg(ap, unsigned int), &fp);
      break;
    case 'u':
      if(ll)
        total += emit_u64(cb, aux, va_arg(ap, uint64_t), &fp, 0);
      else
        total += emit_u32(cb, aux, va_arg(ap, unsigned int), &fp, 0);
      break;
    case 'f':
      ap = fmt_double(ap, tmp, sizeof(tmp));
      total += emit_str(cb, aux, tmp, &fp);
      break;
    case 'p':
      total += cb(aux, "0x", 2);
      total += emit_x32(cb, aux, (intptr_t)va_arg(ap, void *), &fp);
      break;
    }
    s = fmt;
  }

  if(s != fmt)
    total += cb(aux, s, fmt - s);

  return total;
}




static size_t
stream_fmt(void *arg, const char *buf, size_t len)
{
  stream_t *s = arg;
  s->write(s, buf, len);
  return len;
}


int
vstprintf(stream_t *s, const char *fmt, va_list ap)
{
  if(fmt == NULL) {
    s->write(s, NULL, 0);
    return 0;
  }

  return fmtv(stream_fmt, s, fmt, ap);
}

int
stprintf(stream_t *s, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int x = vstprintf(s, fmt, ap);
  va_end(ap);
  return x;
}





#if WITH_MAIN

static size_t
fmt(fmtcb_t *cb, void *aux, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  size_t r = fmtv(cb, aux, fmt, ap);
  va_end(ap);
  return r;
}


#include <unistd.h>
#include <stdlib.h>

size_t
dump_stdout(void *aux, const char *s, size_t len)
{
#if 0
  if(write(1, "|", 1) != 1)
    exit(3);
#endif
  if(write(1, s, len) != len)
    exit(3);
  return len;
}


int
main(int argc, char **argv)
{
  fmt(dump_stdout, NULL, "hej123\n");
  fmt(dump_stdout, NULL, "hej%%123\n");
  fmt(dump_stdout, NULL, "%%%%%%\n");
  fmt(dump_stdout, NULL, "%c%c%c\n", 'a', 'b', 'c');
  fmt(dump_stdout, NULL, "my name is %s\n", "mios");
  fmt(dump_stdout, NULL, "my name is %s\n", NULL);
  fmt(dump_stdout, NULL, "c0dedbad = 0x%x\n", 0xc0dedbad);
  fmt(dump_stdout, NULL, "0 = 0x%x\n", 0);
  fmt(dump_stdout, NULL, "%020x\n", 0x4489);
  fmt(dump_stdout, NULL, "10 = %d\n", 10);
  fmt(dump_stdout, NULL, "9999 = %d\n", 9999);
  fmt(dump_stdout, NULL, "-555 = %d\n", -555);
}
#else


stream_t *stdio;


int
getchar(void)
{
  if(stdio == NULL || stdio->read == NULL)
    return -1;
  char c;
  stdio->read(stdio, &c, 1, STREAM_READ_WAIT_ONE);
  return c;
}



static size_t
stdout_cb(void *aux, const char *s, size_t len)
{
  if(stdio != NULL)
    stdio->write(stdio, s, len);
  return len;
}


int
vprintf(const char *format, va_list ap)
{
  return fmtv(stdout_cb, NULL, format, ap);
}


int
printf(const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  int r = fmtv(stdout_cb, NULL, format, ap);
  va_end(ap);
  return r;
}

int
putchar(int c)
{
  if(stdio) {
    char s8 = c;
    stdio->write(stdio, &s8, 1);
  }
  return c;
}


int
puts(const char *s)
{
  if(stdio) {
    size_t len = strlen(s);
    stdio->write(stdio, s, len);
    stdio->write(stdio, "\n", 1);
  }
  return 0;
}



typedef struct {
  char *ptr;
  size_t size;
  size_t used;
} snbuf_t;


static size_t
snbuf_cb(void *aux, const char *s, size_t len)
{
  snbuf_t *b = aux;

  size_t avail = b->size - b->used;
  if(avail > 0) {

    size_t to_copy = len;
    if(avail < to_copy)
      to_copy = avail;

    memcpy(b->ptr + b->used, s, to_copy);

    b->used += to_copy;
  }
  return len;
}



int
snprintf(char *str, size_t size, const char *format, ...)
{
  snbuf_t buf = {str, size};

  va_list ap;
  va_start(ap, format);
  int r = fmtv(snbuf_cb, &buf, format, ap);
  va_end(ap);

  if(buf.size > 0)
    str[buf.used] = 0;
  return r;
}

#endif
