#pragma once

#include <stdint.h>

#define NV_ADDRESS_MAP_AON_AST_0_BASE 0x0c040000
#define NV_ADDRESS_MAP_AON_AST_1_BASE 0x0c050000

void
ast_set_region(uint32_t ast_base,
               uint32_t region,
               uint64_t phys_addr,
               uint32_t local_addr,
               uint32_t size,
               uint8_t streamid);
