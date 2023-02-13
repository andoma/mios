#pragma once

// PRNG from http://burtleburtle.net/bob/rand/smallprng.html (Public Domain)

#define rot(x,k) (((x)<<(k))|((x)>>(32-(k))))

typedef struct { uint32_t a; uint32_t b; uint32_t c; uint32_t d; } prng_t;


static inline uint32_t
prng_get(prng_t *x, uint32_t seed)
{
  uint32_t e = x->a - rot(x->b, 27);
  x->a = x->b ^ rot(x->c, 17) ^ seed;
  x->b = x->c + x->d;
  x->c = x->d + e;
  x->d = e + x->a;
  return x->d;
}

