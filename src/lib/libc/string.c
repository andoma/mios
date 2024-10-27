#include <string.h>
#include <inttypes.h>
#include <malloc.h>

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
  char *d = dest;
  const char *s = src;
  const char *e = s + n;

  if(n >= 8 && ((intptr_t)d & 3) == 0 && ((intptr_t)s & 3) == 0) {
    size_t words = n >> 2;
    int *d32 = dest;
    const int *s32 = src;

    const int *e32 = s32 + words;
    while(s32 != e32) {
      *d32++ = *s32++;
    }
    s = (const char *)s32;
    d = (char *)d32;
  }

  while(s != e) {
    *d++ = *s++;
  }
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

static int
tolower(int c)
{
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}


int
strcasecmp(const char *s1, const char *s2)
{
  while(*s1 && (tolower(*s1) == tolower(*s2))) {
    s1++;
    s2++;
  }
  return tolower(*(const unsigned char *)s1) - tolower(*(const unsigned char *)s2);
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


const char *
strtbl(const char *str, size_t index)
{
  while(1) {
    if(!index)
      return str;
    index--;
    size_t n = strlen(str);
    if(n == 0)
      return "???";
    str += n + 1;
  }
}


char *
strcpy(char *dst, const char *src)
{
  char *r = dst;
  while(1) {
    *dst = *src;
    if(*src == 0)
      break;
    src++;
    dst++;
  }
  return r;
}


char *
strchr(const char *s, int c)
{
  while(*s) {
    if((char)c == *s)
      return (char *)s;
    s++;
  }
  return NULL;
}


size_t
strspn(const char *s, const char *accept)
{
  size_t i = 0;
  while(s[i]) {
    if(strchr(accept, s[i]) == NULL)
      break;
    i++;
  }
  return i;
}


size_t
strcspn(const char *s, const char *reject)
{
  size_t i = 0;
  while(s[i]) {
    if(strchr(reject, s[i]) != NULL)
      break;
    i++;
  }
  return i;
}

char *
strdup(const char *line)
{
  size_t len = strlen(line);
  char *new = xalloc(len + 1, 1, MEM_MAY_FAIL);
  if (new == NULL)
    return NULL;
  strlcpy(new, line, len + 1);
  return new;
}
