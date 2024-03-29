#pragma once

LIST_HEAD(mbus_flow_list, mbus_flow);

typedef struct mbus_flow {

  LIST_ENTRY(mbus_flow) mf_link;

  union {
    struct {
      uint16_t mf_flow;
      uint8_t mf_remote_addr;
    };
    uint32_t mf_tag;
  };

  pbuf_t *(*mf_input)(struct mbus_flow *mf, pbuf_t *pb);

} mbus_flow_t;

mbus_flow_t *mbus_flow_find(uint8_t remote_addr, uint16_t flow);

void mbus_flow_insert(mbus_flow_t *mf);

void mbus_flow_remove(mbus_flow_t *mf);
