#include <string.h>

static void
bcopy(const void *src, void *dest, size_t len)
{

  if(dest < src) {
    const char *s = src;
    const char *e = src + len;
    char *d = dest;

    while(s != e) {
      *d++ = *s++;
    }

  } else {

    const char *s = src + len;
    const char *b = src;
    char *d = dest + len;
    do {
      *--d = *--s;
    } while(b != s);
  }
}


int
memcmp(const void *str1, const void *str2, size_t count)
{
  const unsigned char *s1 = (const unsigned char *)str1;
  const unsigned char *s2 = (const unsigned char *)str2;

  while(count-- > 0) {
    if(*s1++ != *s2++) {
      return s1[-1] < s2[-1] ? -1 : 1;
    }
  }
  return 0;
}

void *
memcpy(void *dest, const void *src, size_t n)
{
  bcopy(src, dest, n);
  return dest;
}

void *
memmove(void *dest, const void *src, size_t n)
{
  bcopy(src, dest, n);
  return dest;
}


void *
memset(void *dest, int val, size_t len)
{
  unsigned char *ptr = (unsigned char *)dest;
  while(len-- > 0)
    *ptr++ = val;
  return dest;
}

size_t
strlen(const char *s)
{
  size_t r = 0;
  while(*s++) {
    r++;
  }
  return r;
}

int
strcmp(const char *s1, const char *s2)
{
  while(*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

size_t
strlcpy(char * restrict dst, const char * restrict src, size_t siz)
{
  const char *s0 = src;

  while(siz > 1) {
    if(*src == 0)
      break;
    *dst++ = *src++;
    siz--;
  }

  if(siz > 0) {
    *dst = 0;
    siz--;
  }

  return (src - s0) + strlen(src);
}

int
atoi(const char *s)
{
  int m = 1;
  int r = 0;
  while(*s && *s <= 32)
    s++;

  if(!*s)
    return 0;

  if(*s == '-') {
    m = -1;
    s++;
  }
  while(*s >= '0' && *s <= '9') {
    r = r * 10 + *s - '0';
    s++;
  }
  return r * m;
}
