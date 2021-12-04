#include "ntcpoly.h"

int ntcpoly_compute(int32_t x, const struct ntcpoly *np) {
  const int32_t x2 = (x * x) >> np->r;
  const int32_t x3 = (x2 * x) >> np->r;
  const int32_t t0 = np->K0;
  const int32_t t1 = (np->K1 * x)  >> np->s1;
  const int32_t t2 = (np->K2 * x2) >> np->s2;
  const int32_t t3 = (np->K3 * x3) >> np->s3;
  return (t0 + t1 + t2 + t3) >> np->s0;
}
