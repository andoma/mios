#pragma once

#define BPMP_MRQ_RESET           20U
#define BPMP_MRQ_CLK		 22U
#define BPMP_MRQ_RINGBUF_CONSOLE 65U
#define BPMP_MRQ_PG		 66U
#define BPMP_MRQ_UPHY		 69U


struct bpmp_mrq_reset_req {
  uint32_t cmd;
  uint32_t reset_id;
} __attribute__((packed));

enum {
  BPMP_CMD_RESET_ASSERT = 1,
  BPMP_CMD_RESET_DEASSERT = 2,
  BPMP_CMD_RESET_TOGGLE = 3,
  BPMP_CMD_RESET_GET_MAX_ID = 4,
};

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


// --- Powergating ---


enum {
  BPMP_CMD_PG_QUERY_ABI = 0,
  BPMP_CMD_PG_SET_STATE = 1,
  BPMP_CMD_PG_GET_STATE = 2,
  BPMP_CMD_PG_GET_NAME = 3,
  BPMP_CMD_PG_GET_MAX_ID = 4,
};

struct bpmp_mrq_pg_req {

  uint32_t cmd;
  uint32_t id;
  uint32_t state;

} __attribute__((packed));


struct bpmp_mrq_pg_resp {

  union {
    uint32_t u32;
    uint8_t name[40];
  };
};



// --- UPHY ---


enum {
	BPMP_CMD_UPHY_PCIE_LANE_MARGIN_CONTROL = 1,
	BPMP_CMD_UPHY_PCIE_LANE_MARGIN_STATUS = 2,
	BPMP_CMD_UPHY_PCIE_EP_CONTROLLER_PLL_INIT = 3,
	BPMP_CMD_UPHY_PCIE_CONTROLLER_STATE = 4,
	BPMP_CMD_UPHY_PCIE_EP_CONTROLLER_PLL_OFF = 5,
	BPMP_CMD_UPHY_DISPLAY_PORT_INIT = 6,
	BPMP_CMD_UPHY_DISPLAY_PORT_OFF = 7,
	BPMP_CMD_UPHY_XUSB_DYN_LANES_RESTORE = 8,
};

struct bpmp_mrq_uphy_request {
  uint16_t lane;
  uint16_t cmd;

  union {
    struct {
      uint8_t controller;
      uint8_t enable;
    } pcie;
  };
};

// --- Console ---

enum {
  BPMP_CMD_RINGBUF_CONSOLE_QUERY_ABI = 0,
  BPMP_CMD_RINGBUF_CONSOLE_READ = 1,
  BPMP_CMD_RINGBUF_CONSOLE_WRITE = 2,
  BPMP_CMD_RINGBUF_CONSOLE_GET_FIFO = 3,
};



struct bpmp_mrq_ringbuf_console_host_to_bpmp_request {
  uint32_t type;
  union {
    uint8_t read_len;
  };
};

union bpmp_mrq_ringbuf_console_bpmp_to_host_response {
  struct {
    uint32_t len;
    uint8_t bytes[120 - 4];
  } read;
};
