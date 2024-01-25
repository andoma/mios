#pragma once

// Global update hooks

typedef enum {
  GHOOK_DHCP_UPDATE,
} ghook_type_t;

typedef void (*ghook_t)(ghook_type_t type, ...);

#define GHOOK(cb)                                                  \
  static const ghook_t MIOS_JOIN(ghook, __LINE__) __attribute__ ((used, section("ghook"))) = (void *)cb;

#define ghook_invoke(type...) do { \
  extern unsigned long _ghook_array_begin; \
  extern unsigned long _ghook_array_end; \
  const ghook_t *h = (void *)&_ghook_array_begin; \
  for(; h != (const void *)&_ghook_array_end; h++) \
    (*h)(type); \
  } while(0)
