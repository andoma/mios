#pragma once

#define BPMP_MRQ_CLK		22U


enum {
  BPMP_CMD_CLK_GET_RATE = 1,
  BPMP_CMD_CLK_SET_RATE = 2,
  BPMP_CMD_CLK_ROUND_RATE = 3,
  BPMP_CMD_CLK_GET_PARENT = 4,
  BPMP_CMD_CLK_SET_PARENT = 5,
  BPMP_CMD_CLK_IS_ENABLED = 6,
  BPMP_CMD_CLK_ENABLE = 7,
  BPMP_CMD_CLK_DISABLE = 8,
  BPMP_CMD_CLK_PROPERTIES = 9,
  BPMP_CMD_CLK_POSSIBLE_PARENTS = 10,
  BPMP_CMD_CLK_NUM_POSSIBLE_PARENTS = 11,
  BPMP_CMD_CLK_GET_POSSIBLE_PARENT = 12,
  BPMP_CMD_CLK_RESET_REFCOUNTS = 13,
  BPMP_CMD_CLK_GET_ALL_INFO = 14,
  BPMP_CMD_CLK_GET_MAX_CLK_ID = 15,
  BPMP_CMD_CLK_GET_FMAX_AT_VMIN = 16,
};

#define BPMP_MRQ_CLK_NAME_MAXLEN	40U
#define BPMP_MRQ_CLK_MAX_PARENTS	16U

struct bpmp_mrq_clk_req {

  uint32_t id : 24;
  uint32_t cmd : 8;

} __attribute__((packed));

struct bpmp_mrq_clk_resp {

  union {

    uint32_t u32;

    struct {
      uint32_t flags;
      uint32_t parent;
      uint32_t parents[BPMP_MRQ_CLK_MAX_PARENTS];
      uint8_t num_parents;
      uint8_t name[BPMP_MRQ_CLK_NAME_MAXLEN];

    } all_info;

  };

} __attribute__((packed));
