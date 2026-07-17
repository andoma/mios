#pragma once

#include <stdint.h>

// BLE Security Manager Protocol, l2cap fixed channel 6 (Core v5.3 Vol 3 Part H).

#define L2CAP_CID_SMP  0x0006

// SMP command codes.
#define SMP_PAIRING_REQUEST     0x01
#define SMP_PAIRING_RESPONSE    0x02
#define SMP_PAIRING_CONFIRM     0x03
#define SMP_PAIRING_RANDOM      0x04
#define SMP_PAIRING_FAILED      0x05
#define SMP_ENCRYPTION_INFO     0x06 // LTK (bonding)
#define SMP_CENTRAL_IDENT       0x07 // EDIV, Rand (bonding)
#define SMP_IDENTITY_INFO       0x08 // IRK
#define SMP_IDENTITY_ADDR_INFO  0x09
#define SMP_SIGNING_INFO        0x0a // CSRK
#define SMP_SECURITY_REQUEST    0x0b

// Pairing Failed reasons.
#define SMP_ERR_PASSKEY_ENTRY   0x01
#define SMP_ERR_OOB_NOT_AVAIL   0x02
#define SMP_ERR_AUTH_REQ        0x03
#define SMP_ERR_CONFIRM_FAILED  0x04
#define SMP_ERR_PAIRING_NOTSUPP 0x05
#define SMP_ERR_ENC_KEY_SIZE    0x06
#define SMP_ERR_CMD_NOTSUPP     0x07
#define SMP_ERR_UNSPECIFIED     0x08

// IO capabilities.
#define SMP_IO_NO_INPUT_NO_OUTPUT 0x03

// Key distribution flags (init_key_dist / resp_key_dist).
#define SMP_DIST_ENC   0x01 // LTK / EDIV / Rand
#define SMP_DIST_ID    0x02 // IRK + identity address
#define SMP_DIST_SIGN  0x04 // CSRK
#define SMP_DIST_LINK  0x08 // LE Secure Connections link key

// AuthReq flags.
#define SMP_AUTH_BONDING        0x01
#define SMP_AUTH_MITM           0x04
#define SMP_AUTH_SC             0x08

// Pairing Request / Response payload (7 bytes including the code byte).
typedef struct {
  uint8_t code;
  uint8_t io_capability;
  uint8_t oob_data_flag;
  uint8_t auth_req;
  uint8_t max_enc_key_size;
  uint8_t init_key_dist;
  uint8_t resp_key_dist;
} __attribute__((packed)) smp_pairing_t;
