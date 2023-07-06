#pragma once

#include <stdint.h>


#define L2CAP_CID_ATT		0x0004
#define L2CAP_CID_LE_SIGNALING	0x0005


#define L2CAP_DISCONNECTION_REQ                0x06
#define L2CAP_DISCONNECTION_RSP                0x07
#define L2CAP_LE_CREDIT_BASED_CONNECTION_REQ   0x14
#define L2CAP_LE_CREDIT_BASED_CONNECTION_RSP   0x15
#define L2CAP_FLOW_CONTROL_CREDIT_IND          0x16

#define L2CAP_CON_OK              0x0
#define L2CAP_CON_NO_PSM          0x2
#define L2CAP_CON_NO_RESOURCES    0x4


#define ATT_ERROR_RSP              0x01
#define ATT_EXCHANGE_MTU_REQ       0x02
#define ATT_EXCHANGE_MTU_RSP       0x03
#define ATT_READ_BY_TYPE_REQ       0x08
#define ATT_READ_REQ               0x0a
#define ATT_READ_RSP               0x0b
#define ATT_READ_BY_GROUP_TYPE_REQ 0x10


typedef struct {
  uint16_t pdu_length;
  uint16_t channel_id;
} l2cap_header_t;



typedef struct {
  uint16_t mtu;
} __attribute__((packed)) l2cap_att_mtu_t;

typedef struct {
  uint16_t starting_handle;
  uint16_t ending_handle;
  uint16_t attribute_type;
} __attribute__((packed)) l2cap_att_read_by_type_req_t;

typedef struct {
  uint16_t handle;
} __attribute__((packed)) l2cap_att_read_t;


typedef struct {
  uint8_t request_opcode;
  uint16_t attribute_handle;
  uint8_t error_code;
} __attribute__((packed)) l2cap_att_error_t;


typedef struct {
  uint8_t code;
  uint8_t identifier;
  uint16_t data_length;
} l2cap_cframe_t;

typedef struct {
  l2cap_cframe_t hdr;
  uint16_t spsm;
  uint16_t src_cid;
  uint16_t mtu;
  uint16_t mps;
  uint16_t initial_credits;
} l2cap_le_credit_based_connection_req_t;

typedef struct {
  l2cap_cframe_t hdr;
  uint16_t dst_cid;
  uint16_t mtu;
  uint16_t mps;
  uint16_t initial_credits;
  uint16_t result;
} l2cap_le_credit_based_connection_rsp_t;

typedef struct {
  l2cap_cframe_t hdr;
  uint16_t dst_cid;
  uint16_t src_cid;
} l2cap_disconnection_req_t;

typedef struct {
  l2cap_cframe_t hdr;
  uint16_t dst_cid;
  uint16_t src_cid;
} l2cap_disconnection_rsp_t;

typedef struct {
  l2cap_cframe_t hdr;
  uint16_t cid;
  uint16_t credits;
} l2cap_flow_control_credit_ind_t;

