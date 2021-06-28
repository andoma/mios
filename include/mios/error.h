#pragma once

typedef enum {
  ERR_OK                    = 0,
  ERR_NOT_IMPLEMENTED       = -1,
  ERR_TIMEOUT               = -2,
  ERR_OPERATION_FAILED      = -3,
  ERR_TX                    = -4,
  ERR_RX                    = -5,
  ERR_NOT_READY             = -6,
  ERR_NO_BUFFER             = -7,
  ERR_MTU_EXCEEDED          = -8,
  ERR_INVALID_ID            = -9,
  ERR_DMA_ERROR             = -10,
  ERR_BUS_ERROR             = -11,
  ERR_ARBITRATION_LOST      = -12,
  ERR_BAD_STATE             = -13,
  ERR_INVALID_ADDRESS       = -14,
  ERR_NO_DEVICE             = -15,
  ERR_MISMATCH              = -16,
  ERR_NOT_FOUND             = -17,
  ERR_CHECKSUM_ERROR        = -18,

} error_t;

