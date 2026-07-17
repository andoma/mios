#pragma once

// Minimal stand-in for the nrfx/CMSIS headers the MPSL/SDC API headers
// include. They only need the IRQn_Type typedef (passed by value, so a
// plain int matches the AAPCS ABI of the prebuilt libraries) and the CMSIS
// struct attribute macros.
typedef int IRQn_Type;

#ifndef __PACKED
#define __PACKED __attribute__((packed))
#endif

#ifndef __ALIGN
#define __ALIGN(n) __attribute__((aligned(n)))
#endif
