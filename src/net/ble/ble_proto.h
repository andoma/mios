#pragma once

#include <stdint.h>

// From Bluertooth Core Specification 5.3 | Vol 6, Part B


#define ADV_IND         0  // 2.3.1.1 ADV_IND
#define ADV_DIRECT_IND  1
#define ADV_NONCONN_IND 2
#define SCAN_REQ        3
#define SCAN_RSP        4
#define CONNECT_IND     5  // 2.3.3.1 CONNECT_IND
#define ADV_SCAN_IND    6
#define CONNECT_RSP     8



#define ADV_RFU    0x10
#define ADV_CHSEL  0x20
#define ADV_TXADD  0x40
#define ADV_RXADD  0x80

struct lldata {             // 2.3.3.1 CONNECT_IND (Figure 2.13)
  uint32_t access_addr;
  uint8_t crcinit[3];
  uint8_t win_size;         // unit: 1.25ms
  uint16_t win_offset;      // unit: 1.25ms
  uint16_t interval;        // unit: 1.25ms
  uint16_t latency;         // unit: ???
  uint16_t timeout;         // unit: 10ms
  uint8_t channel_mask[5];
  uint8_t hop_sca;
};


#define DATA_LLID 0x3
#define DATA_NESN 0x4           // nextExpectedSeqNum
#define DATA_SN   0x8           // transmitSeqNum
#define DATA_MD   0x10
#define DATA_CP   0x20
#define DATA_RFU  0xc0


#define LL_CONNECTION_UPDATE_IND  0x00
#define LL_CHANNEL_MAP_IND        0x01
#define LL_TERMINATE_IND          0x02
#define LL_UNKNOWN_RSP            0x07
#define LL_FEATURE_REQ            0x08
#define LL_FEATURE_RSP            0x09
#define LL_VERSION_IND            0x0c
#define LL_PERIPHERAL_FEATURE_REQ 0x0e
#define LL_LENGTH_REQ             0x14
#define LL_LENGTH_RSP             0x15
#define LL_CHANNEL_REPORTING_IND  0x28

