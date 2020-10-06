#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

static int dbl2str(char *buf, size_t bufsize, double realvalue, int precision);

typedef size_t (fmtcb_t)(void *aux, const char *s, size_t len);

typedef struct {
  int16_t width;
  unsigned char lz:1;
  unsigned char la:1;
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
  total += cb(aux, buf + 10 - digits, digits);

  if(fp->la)
    total += emit_repeated_char(cb, aux, pad, ' ');

  return total;
}


static size_t  __attribute__((noinline))
emit_s32(fmtcb_t *cb, void *aux, int x,
         const fmtparam_t *fp)
{
  if(x < 0)
    return emit_u32(cb, aux, -x, fp, 1);
  else
    return emit_u32(cb, aux, x, fp, 0);
}


static size_t  __attribute__((noinline))
emit_double(fmtcb_t *cb, void *aux, double v,
           const fmtparam_t *fp)
{
  char tmp[32];
  dbl2str(tmp, sizeof(tmp), v, -1);
  return emit_str(cb, aux, tmp, fp);
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
    fp.lz = 0;
    fp.la = 0;

    if(*fmt == '0') {
      fp.lz = 1;
      fmt++;
    } else if(*fmt == '-') {
      fp.la = 1;
      fmt++;
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
      total += emit_str(cb, aux, va_arg(ap, const char *), &fp);
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
    case 'f':
      total += emit_double(cb, aux, va_arg(ap, double), &fp);
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










/*
** The code that follow is based on "printf" code that dates from the
** 1980s. It is in the public domain.  The original comments are
** included here for completeness.  They are very out-of-date but
** might be useful as an historical reference.
*/

static char
getdigit(double *val, int *cnt)
{
  int digit;
  double d;
  if( (*cnt)++ >= 16 ) return '0';
  digit = (int)*val;
  d = digit;
  digit += '0';
  *val = (*val - d)*10.0;
  return (char)digit;
}

#define xGENERIC 0
#define xFLOAT 1
#define xEXP 2


int
dbl2str(char *buf, size_t bufsize, double realvalue, int precision)
{
  char *bufpt;
  char prefix;
  char xtype = xGENERIC;
  int idx, exp, e2;
  double rounder;
  char flag_exp;
  char flag_rtz;
  char flag_dp;
  char flag_alternateform = 0;
  char flag_altform2 = 0;
  int nsd;

  if(bufsize < 8)
    return -1;

  if( precision<0 ) precision = 20;         /* Set default precision */
  if( precision>bufsize/2-10 ) precision = bufsize/2-10;
  if( realvalue<0.0 ){
    realvalue = -realvalue;
    prefix = '-';
  }else{
    prefix = 0;
  }
  if( xtype==xGENERIC && precision>0 ) precision--;
  for(idx=precision, rounder=0.5; idx>0; idx--, rounder*=0.1){}

  if( xtype==xFLOAT ) realvalue += rounder;
  /* Normalize realvalue to within 10.0 > realvalue >= 1.0 */
  exp = 0;

#if 0
  if(isnan(realvalue)) {
    memcpy(buf, "NaN", 4);
    return 0;
  }
#endif

  if( realvalue>0.0 ){
    while( realvalue>=1e32 && exp<=350 ){ realvalue *= 1e-32; exp+=32; }
    while( realvalue>=1e8 && exp<=350 ){ realvalue *= 1e-8; exp+=8; }
    while( realvalue>=10.0 && exp<=350 ){ realvalue *= 0.1; exp++; }
    while( realvalue<1e-8 ){ realvalue *= 1e8; exp-=8; }
    while( realvalue<1.0 ){ realvalue *= 10.0; exp--; }
    if( exp>350 ){
      if( prefix=='-' ){
	memcpy(buf, "-Inf", 5);
      }else{
	memcpy(buf, "Inf", 4);
      }
      return 0;
    }
  }
  bufpt = buf;

  /*
  ** If the field type is etGENERIC, then convert to either etEXP
  ** or etFLOAT, as appropriate.
  */
  flag_exp = xtype==xEXP;
  if( xtype != xFLOAT ){
    realvalue += rounder;
    if( realvalue>=10.0 ){ realvalue *= 0.1; exp++; }
  }
  if( xtype==xGENERIC ){
    flag_rtz = !flag_alternateform;
    if( exp<-4 || exp>precision ){
      xtype = xEXP;
    }else{
      precision = precision - exp;
      xtype = xFLOAT;
    }
  }else{
    flag_rtz = 0;
  }
  if( xtype==xEXP ){
    e2 = 0;
  }else{
    e2 = exp;
  }
  nsd = 0;
  flag_dp = (precision>0 ?1:0) | flag_alternateform | flag_altform2;
  /* The sign in front of the number */
  if( prefix ){
    *(bufpt++) = prefix;
  }
  /* Digits prior to the decimal point */
  if( e2<0 ){
    *(bufpt++) = '0';
  }else{
    for(; e2>=0; e2--){
      *(bufpt++) = getdigit(&realvalue,&nsd);
    }
  }
  /* The decimal point */
  if( flag_dp ){
    *(bufpt++) = '.';
  }
  /* "0" digits after the decimal point but before the first
  ** significant digit of the number */
  for(e2++; e2<0; precision--, e2++){
    assert( precision>0 );
    *(bufpt++) = '0';
  }
  /* Significant digits after the decimal point */
  while( (precision--)>0 ){
    *(bufpt++) = getdigit(&realvalue,&nsd);
  }

  /* Remove trailing zeros and the "." if no digits follow the "." */
  if( flag_rtz && flag_dp ){
    while( bufpt[-1]=='0' ) *(--bufpt) = 0;
    assert( bufpt>buf );
    if( bufpt[-1]=='.' ){
      if( flag_altform2 ){
	*(bufpt++) = '0';
      }else{
	*(--bufpt) = 0;
      }
    }
  }
  /* Add the "eNNN" suffix */
  if( flag_exp || xtype==xEXP ){
    *(bufpt++) = 'e';
    if( exp<0 ){
      *(bufpt++) = '-'; exp = -exp;
    }else{
      *(bufpt++) = '+';
    }
    if( exp>=100 ){
      *(bufpt++) = (char)((exp/100)+'0');        /* 100's digit */
      exp %= 100;
    }
    *(bufpt++) = (char)(exp/10+'0');             /* 10's digit */
    *(bufpt++) = (char)(exp%10+'0');             /* 1's digit */
  }
  *bufpt = 0;
  return 0;
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

int
putchar(int c)
{
  if(stdio_putc) {
    stdio_putc(stdio_putc_arg, c);
  }
  return c;
}


int
puts(const char *s)
{
  if(stdio_putc) {
    while(*s) {
      stdio_putc(stdio_putc_arg, *s);
      s++;
    }
    stdio_putc(stdio_putc_arg, '\n');
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
