#include "t234ccplex_ari.h"

#include <stdio.h>
#include <mios/mios.h>
#include "reg.h"

#define ARI_BASE 0xe100000

#define ARI_REQUEST             0x00
#define ARI_REQUEST_EVENT_MASK  0x08
#define ARI_STATUS              0x10
#define ARI_REQUEST_DATA_LO     0x18
#define ARI_REQUEST_DATA_HI     0x20
#define ARI_RESPONSE_DATA_LO    0x28
#define ARI_RESPONSE_DATA_HI    0x30

#define ARI_REQUEST_VALID_BIT  (1U << 8)
#define ARI_REQUEST_KILL_BIT   (1U << 9)
#define ARI_REQUEST_NS_BIT     (1U << 31)


#define ARI_REQ_ERROR_STATUS_MASK   0xFC
#define ARI_REQ_ERROR_STATUS_SHIFT  2
#define ARI_REQ_NO_ERROR            0
#define ARI_REQ_REQUEST_KILLED      1
#define ARI_REQ_NS_ERROR            2
#define ARI_REQ_EXECUTION_ERROR     0x3F


error_t
ari_cmd(uint32_t cmd, uint64_t in, uint64_t *out)
{
  uint64_t v;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(v));

  int linear_core_id = ((v >> 16) & 0xff) * 4 + ((v >> 8) & 0xff);
  uint64_t ari_base = ARI_BASE + linear_core_id * 0x10000;

  reg_wr(ari_base + ARI_RESPONSE_DATA_LO, 0);
  reg_wr(ari_base + ARI_RESPONSE_DATA_HI, 0);
  reg_wr(ari_base + ARI_REQUEST_DATA_LO, in);
  reg_wr(ari_base + ARI_REQUEST_DATA_HI, in >> 32);
  reg_wr(ari_base + ARI_REQUEST_EVENT_MASK, 0);
  reg_wr(ari_base + ARI_REQUEST, cmd | ARI_REQUEST_VALID_BIT);

  for(int i = 0; i < 10000; i++) {
    uint32_t status = reg_rd(ari_base + ARI_STATUS);

    if(status & 3)
      continue;

    uint32_t err = (status >> 2) & 0x3f;
    if(err)
      return ERR_OPERATION_FAILED;
    *out =
      reg_rd(ari_base + ARI_RESPONSE_DATA_LO) |
      (((uint64_t)reg_rd(ari_base + ARI_RESPONSE_DATA_HI)) << 32);
    return 0;
  }
  return ERR_TIMEOUT;
}

static void __attribute__((constructor(900)))
ari_init(void)
{
  uint64_t ver;
  error_t err = ari_cmd(TEGRA_ARI_VERSION_CMD, 0, &ver);
  if(!err) {
    printf("t234 ARI version %d.%d\n",
           (uint32_t)(ver >> 32),
           (uint32_t)ver);
  } else {
    panic("t234 ARI not responding: %s", error_to_string(err));
  }

  uint64_t cores;
  err = ari_cmd(TEGRA_ARI_NUM_CORES_CMD, 0, &cores);
  printf("t234 coremask: 0x%lx\n", (long)cores);
}




