#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

typedef size_t (fmtcb_t)(void *aux, const char *s, size_t len);

typedef struct {
  int width;
  char lz;
} fmtparam_t;


static size_t
emit_str(fmtcb_t *cb, void *aux, const char *str)
{
  if(str == NULL)
    return cb(aux, "(null)", 6);
  else
    return cb(aux, str, strlen(str));
}


static size_t
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
  for(int i = 0; i < pad; i++) {
    total += cb(aux, fp->lz ? "0" : " ", 1);
  }
  if(neg)
    total += cb(aux, "-", 1);
  return cb(aux, buf + 10 - digits, digits) + total;
}


static size_t
emit_s32(fmtcb_t *cb, void *aux, int x,
         const fmtparam_t *fp)
{
  if(x < 0)
    return emit_u32(cb, aux, -x, fp, 1);
  else
    return emit_u32(cb, aux, x, fp, 0);
}


static size_t
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


static int
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

static size_t
fmtv(fmtcb_t *cb, void *aux, const char *fmt, va_list ap)
{
  const char *s = fmt;
  char c;
  size_t total = 0;

  while((c = *fmt++) != 0) {
    if(c != '%')
      continue;

    if(s != fmt)
      total += cb(aux, s, fmt - s - 1);

    fmtparam_t fp;

    if(*fmt == '0') {
      fp.lz = 1;
      fmt++;
    } else {
      fp.lz = 0;
    }

    fp.width = parse_dec(&fmt, -1);

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
      total += emit_str(cb, aux, va_arg(ap, const char *));
      break;
    case 'x':
      total += emit_x32(cb, aux, va_arg(ap, unsigned int), &fp);
      break;
    case 'd':
      total += emit_s32(cb, aux, va_arg(ap, unsigned int), &fp);
      break;
    case 'u':
      total += emit_u32(cb, aux, va_arg(ap, unsigned int), &fp, 0);
      break;
    case 'p':
      total += emit_str(cb, aux, "0x");
      total += emit_x32(cb, aux, (intptr_t)va_arg(ap, void *), &fp);
      break;
    }
    s = fmt;
  }

  if(s != fmt)
    total += cb(aux, s, fmt - s);

  return total;
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


static void *stdio_putc_arg;
static void (*stdio_putc)(void *arg, char c);
static void *stdio_getchar_arg;
static int (*stdio_getchar)(void *arg);


void
init_printf(void *arg, void (*cb)(void *arg, char c))
{
  stdio_putc_arg = arg;
  stdio_putc = cb;
}

void
init_getchar(void *arg, int (*cb)(void *arg))
{
  stdio_getchar_arg = arg;
  stdio_getchar = cb;
}



int
getchar(void)
{
  if(stdio_getchar == NULL)
    return -1;
  return stdio_getchar(stdio_getchar_arg);
}



static size_t
stdout_cb(void *aux, const char *s, size_t len)
{
  if(stdio_putc) {
    for(size_t i = 0; i < len; i++) {
      stdio_putc(stdio_putc_arg, s[i]);
    }
  }
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
