#pragma once

#define PRIx16 "x"
#define PRIx32 "x"
#define PRIu32 "u"
#define PRId32 "d"
#define PRIu16 "u"
#define PRId16 "d"

#if __LONG_WIDTH__ == 32

#define PRIx64 "llx"
#define PRIu64 "llu"
#define PRId64 "lld"

#else

#define PRIx64 "lx"
#define PRIu64 "lu"
#define PRId64 "ld"

#endif

#include <stdint.h>
