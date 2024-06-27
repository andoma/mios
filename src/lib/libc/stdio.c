#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <mios/stream.h>
#include <mios/fmt.h>

#ifdef ENABLE_NET_IPV4
#include <net/net.h>
#endif

#ifdef ENABLE_FIXMATH
#include <fixmath.h>
#endif

typedef struct {
  int16_t width;
  int16_t decimals;
  unsigned char lz:1;
  unsigned char la:1;
  unsigned char plus:1;
  unsigned char sign_pad:1;
#ifdef ENABLE_NET_IPV4
  unsigned char ipv4:1;
#endif
} fmtparam_t;


static size_t __attribute__((noinline))
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
emit_intstr(fmtcb_t *cb, void *aux, const fmtparam_t *fp, int neg,
            const char *end, int digits)
{
  size_t total = 0;

  const int pad = fp->width - (digits + neg);
  if(!fp->la)
    total += emit_repeated_char(cb, aux, pad,
                                fp->lz ? '0' : ' ');

  if(neg)
    total += cb(aux, "-", 1);
  total += cb(aux, end - digits, digits);

  if(fp->la)
    total += emit_repeated_char(cb, aux, pad, ' ');

  return total;

}

static size_t  __attribute__((noinline))
emit_u32(fmtcb_t *cb, void *aux, const fmtparam_t *fp,
         int neg, unsigned int x)
{
  char buf[10];
  int digits = 1;
  for(int i = 0; i < 10; i++, x /= 10) {
    const unsigned int d = x % 10;
    buf[9 - i] = '0' + d;
    if(d)
      digits = i + 1;
  }
  return emit_intstr(cb, aux, fp, neg, buf + sizeof(buf), digits);
}


#ifndef DISABLE_FMT_64BIT
static size_t  __attribute__((noinline))
emit_u64(fmtcb_t *cb, void *aux,
         const fmtparam_t *fp, int neg, uint64_t x)
{
  char buf[20];
  int digits = 1;
  for(int i = 0; i < 20; i++, x /= 10) {
    const unsigned int d = x % 10;
    buf[19 - i] = '0' + d;
    if(d)
      digits = i + 1;
  }

  return emit_intstr(cb, aux, fp, neg, buf + sizeof(buf), digits);
}
#endif

static size_t  __attribute__((noinline))
emit_s32(fmtcb_t *cb, void *aux, const fmtparam_t *fp, int x)
{
#ifdef ENABLE_NET_IPV4
  if(fp->ipv4) {
    size_t r = 0;
    x = ntohl(x);
    char dot = '.';
    r += emit_u32(cb, aux, fp, 0, (x >> 24) & 0xff);
    r += cb(aux, &dot, 1);
    r += emit_u32(cb, aux, fp, 0, (x >> 16) & 0xff);
    r += cb(aux, &dot, 1);
    r += emit_u32(cb, aux, fp, 0, (x >> 8) & 0xff);
    r += cb(aux, &dot, 1);
    r += emit_u32(cb, aux, fp, 0, x & 0xff);
    return r;
  }
#endif
  if(x < 0)
    return emit_u32(cb, aux, fp, 1, -x);
  else
    return emit_u32(cb, aux, fp, 0, x);
}



#ifdef ENABLE_FIXMATH
static size_t  __attribute__((noinline))
emit_fix16(fmtcb_t *cb, void *aux, const fmtparam_t *fp, int x)
{
  char buf[13];
  fix16_to_str(x, buf, fp->decimals != -1 ? fp->decimals : 5);
  return emit_str(cb, aux, buf, fp);
}
#endif

#ifndef DISABLE_FMT_64BIT
static size_t  __attribute__((noinline))
emit_s64(fmtcb_t *cb, void *aux,
         const fmtparam_t *fp, int64_t x)
{
  if(x < 0)
    return emit_u64(cb, aux, fp, 1, -x);
  else
    return emit_u64(cb, aux, fp, 0, x);
}
#endif

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

#ifdef ENABLE_MATH
static int
flt_count_output_chars(int sign, int e10, int significands, const fmtparam_t *fp)
{
  int count = 0;
  if(sign || fp->plus || fp->sign_pad)
    count++;

  if(e10 > 0) {
    significands += e10;
  } else {
    count += 2;

    for(int i = 0; i > e10; i--) {
      count += 1;
      significands--;
    }
    if(significands <= 0)
      return count;
  }

  for(int i = 0; i < significands; i++) {

    count ++;
    e10--;
    if(e10 == 0) {
      count++;
    }
  }
  return count;
}


static size_t
float_to_str(fmtcb_t *cb, void *aux,
             uint64_t bits,
             const fmtparam_t *fp)
{
  int e10 = 0;

  const int sign = bits >> 63;
  uint64_t mantissa = bits & ((1ULL << 52) - 1);
  int e2 = (bits >> 52) & 0x7ff;

  if(e2 == 2047) {
    const char *str;
    if(mantissa) {
      str = "nan";
    } else if(sign) {
      str = "-inf";
    } else {
      str = "+inf";
    }
    return emit_str(cb, aux, str, fp);
  }

  if(e2 == 0) {
    e10 = 1;
    mantissa = 0;
  } else {

    e2 -= 1022;
    mantissa = mantissa << 11;
    mantissa |= 1ULL << 63;

    while(e2 < -2) {
      do {
        mantissa = mantissa >> 1;
        e2++;
      } while((uint32_t)(mantissa >> 32) >= 0x33333333);

      mantissa *= 5;
      e2++;
      e10--;
    }

    while(e2 > 0) {
      mantissa = (2 + mantissa) / 5;
      e2--;
      e10++;

      do {
        mantissa = mantissa << 1;
        e2--;
      } while(!(mantissa & (1ull << 63)));
    }

    mantissa = mantissa >> (4 - e2);
  }
  size_t total = 0;
  int significands = fp->decimals > 0 ? fp->decimals : 6;

  int chars = flt_count_output_chars(sign, e10, significands, fp);
  int pad = fp->width - chars;

  if(!fp->la)
    total += emit_repeated_char(cb, aux, pad, ' ');

  if(sign) {
    total += cb(aux, "-", 1);
  } else if(fp->plus) {
    total += cb(aux, "+", 1);
  } else if(fp->sign_pad) {
    total += cb(aux, " ", 1);
  }

  if(e10 > 0) {
    significands += e10;
  } else {
    total += cb(aux, "0.", 2);

    for(int i = 0; i > e10; i--) {
      total += cb(aux, "0", 1);
      significands--;
    }
    if(significands <= 0)
      goto done;
  }

  uint64_t r = 1ULL << 59;
  for(int i = 0; i < significands; i++)
    r /= 10;
  mantissa += r;

  for(int i = 0; i < significands; i++) {

    mantissa *= 10;
    char c = mantissa >> 60;
    if(c > 9)
      c = 9;
    c += '0';
    total += cb(aux, &c, 1);
    mantissa &= 0x0fffffffffffffffull;
    e10--;
    if(e10 == 0) {
      total += cb(aux, ".", 1);
    }
  }
 done:
  if(fp->la)
    total += emit_repeated_char(cb, aux, pad, ' ');
  return total;
}
#endif


static int __attribute__((noinline))
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

size_t __attribute__((noinline))
fmtv(fmtcb_t *cb, void *aux, const char *fmt, va_list ap)
{
  const char *s = fmt;
  char c;
  size_t total = 0;
#ifdef ENABLE_MATH
  union {
    double dbl;
    uint64_t u64;
  } u;
#endif
  while((c = *fmt) != 0) {
    fmt++;
    if(c != '%')
      continue;

    if(s != fmt) {
      size_t l = fmt - s - 1;
      if(l) {
        total += cb(aux, s, fmt - s - 1);
      }
    }

    fmtparam_t fp = {};

    if(*fmt == '0') {
      fp.lz = 1;
      fmt++;
    } else if(*fmt == '-') {
      fp.la = 1;
      fmt++;
    }

    fp.width = parse_dec(&fmt, -1);

    if(fmt[0] == '.') {
      fmt++;
      fp.decimals = parse_dec(&fmt, -1);
    } else {
      fp.decimals = -1;
    }

#ifdef ENABLE_NET_IPV4
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
#ifndef DISABLE_FMT_64BIT
      if(ll)
        total += emit_s64(cb, aux, &fp, va_arg(ap, uint64_t));
      else
#endif
        total += emit_s32(cb, aux, &fp, va_arg(ap, unsigned int));
      break;
    case 'u':
#ifndef DISABLE_FMT_64BIT
      if(ll)
        total += emit_u64(cb, aux, &fp, 0, va_arg(ap, uint64_t));
      else
#endif
        total += emit_u32(cb, aux, &fp, 0, va_arg(ap, unsigned int));
      break;
#ifdef ENABLE_MATH
    case 'f':
      u.dbl = va_arg(ap, double);
      total += float_to_str(cb, aux, u.u64, &fp);
      break;
#endif
    case 'p':
      total += cb(aux, "0x", 2);
      total += emit_x32(cb, aux, (intptr_t)va_arg(ap, void *), &fp);
      break;
#ifdef ENABLE_FIXMATH
    case 'o':
      total += emit_fix16(cb, aux, &fp, (int)va_arg(ap, int));
      break;
#endif
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
  s->write(s, buf, len, 0);
  return len;
}


int
vstprintf(stream_t *s, const char *fmt, va_list ap)
{
  if(fmt == NULL) {
    s->write(s, NULL, 0, 0);
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


void
sthexdump(stream_t *s, const char *prefix, const void *buf, size_t len,
          uint32_t offset)
{
  int i, j, k;
  const uint8_t *data = buf;

  for(i = 0; i < len; i+= 16) {
    stprintf(s, "%s%s0x%04x: ", prefix ?: "", prefix ? ": " : "", i + offset);

    for(j = 0; j + i < len && j < 16; j++) {
      stprintf(s, "%s%02x ", j==8 ? " " : "", data[i+j]);
    }
    const int cnt = (17 - j) * 3 + (j < 8);
    for(k = 0; k < cnt; k++) {
      stprintf(s, " ");
    }

    for(j = 0; j + i < len && j < 16; j++) {
      char c = data[i+j] < 32 || data[i+j] > 126 ? '.' : data[i+j];
      stprintf(s, "%c", c);
    }
    stprintf(s, "\n");
  }
}

void
hexdump(const char *prefix, const void *data, size_t len)
{
  sthexdump(stdio, prefix, data, len, 0);
}


void
stprintflags(stream_t *s, const char *str, uint32_t flags, const char *sep)
{
  int i = 0;
  int need_sep = 0;
  size_t seplen = strlen(sep);
  while(1) {
    size_t n = strlen(str);
    if(n == 0)
      break;
    if(flags & (1 << i)) {
      if(need_sep) {
        s->write(s, sep, seplen, 0);
      }
      s->write(s, str, n, 0);
      need_sep = 1;
    }
    str += n + 1;
    i++;
  }
}


void
sthexstr(stream_t *s, const void *data, size_t len)
{
  const uint8_t *u8 = data;
  char buf[2];
  for(size_t i = 0; i < len; i++) {
    uint8_t v = u8[i];
    buf[0] = "0123456789abcdef"[v >> 4];
    buf[1] = "0123456789abcdef"[v & 0xf];
    s->write(s, buf, 2, 0);
  }
}


void
bin2hex(char *s, const void *data, size_t len)
{
  const uint8_t *u8 = data;
  for(size_t i = 0; i < len; i++) {
    uint8_t v = u8[i];
    *s++ = "0123456789abcdef"[v >> 4];
    *s++ = "0123456789abcdef"[v & 0xf];
  }
  *s = 0;
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
    stdio->write(stdio, s, len, 0);
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
    stdio->write(stdio, &s8, 1, 0);
  }
  return c;
}


int
puts(const char *s)
{
  if(stdio) {
    size_t len = strlen(s);
    stdio->write(stdio, s, len, 0);
    stdio->write(stdio, "\n", 1, 0);
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
vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
  snbuf_t buf = {str, size};

  int r = fmtv(snbuf_cb, &buf, format, ap);

  if(buf.size > 0)
    str[buf.used] = 0;
  return r;
}


int
snprintf(char *str, size_t size, const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  int r = vsnprintf(str, size, format, ap);
  va_end(ap);
  return r;
}

#endif
