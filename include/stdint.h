#pragma once

typedef __UINT64_TYPE__  uint64_t;
typedef unsigned int     uint32_t;
typedef unsigned short   uint16_t;
typedef unsigned char    uint8_t;

typedef __INT64_TYPE__   int64_t;
typedef signed int       int32_t;
typedef signed short     int16_t;
typedef signed char      int8_t;

typedef __INTPTR_TYPE__  intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;

#define INT8_MIN    (-0x7f - 1)
#define INT16_MIN   (-0x7fff - 1)
#define INT32_MIN   (-0x7fffffff - 1)
#define INT64_MIN   (-__INT64_MAX__  1)

#define INT8_MAX    0x7f
#define INT16_MAX   0x7fff
#define INT32_MAX   0x7fffffff
#define INT64_MAX   __INT64_MAX__

#define UINT8_MAX   0xff
#define UINT16_MAX  0xffff
#define UINT32_MAX  0xffffffff
#define UINT64_MAX  __UINT64_MAX__
