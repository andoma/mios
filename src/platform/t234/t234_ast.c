#include "t234_ast.h"

#include "reg.h"

#define AST_INT_LO(x) (0x100 + (x) * 0x20 + 0x00)
#define AST_INT_HI(x) (0x100 + (x) * 0x20 + 0x04)
#define AST_MSK_LO(x) (0x100 + (x) * 0x20 + 0x08)
#define AST_MSK_HI(x) (0x100 + (x) * 0x20 + 0x0c)
#define AST_EXT_LO(x) (0x100 + (x) * 0x20 + 0x10)
#define AST_EXT_HI(x) (0x100 + (x) * 0x20 + 0x14)
#define AST_CTRL(x)   (0x100 + (x) * 0x20 + 0x18)

void
ast_set_region(uint32_t ast_base,
               uint32_t region,
               uint64_t phys_addr,
               uint32_t local_addr,
               uint32_t size,
               uint8_t streamid)
{
  const uint64_t mask = size - 1;

  uint32_t control = 0;
  control |= (streamid << 15);
  control |= (1 << 2); // Enable snooping
  reg_wr(ast_base + AST_CTRL(region), control);

  reg_wr(ast_base + AST_MSK_HI(region), mask >> 32);
  reg_wr(ast_base + AST_MSK_LO(region), mask);
  reg_wr(ast_base + AST_EXT_HI(region), phys_addr >> 32);
  reg_wr(ast_base + AST_EXT_LO(region), phys_addr);
  reg_wr(ast_base + AST_INT_HI(region), 0);
  reg_wr(ast_base + AST_INT_LO(region), local_addr | 1);
}


