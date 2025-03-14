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
  ERR_DMA_XFER              = -10,
  ERR_BUS_ERROR             = -11,
  ERR_ARBITRATION_LOST      = -12,
  ERR_BAD_STATE             = -13,
  ERR_INVALID_ADDRESS       = -14,
  ERR_NO_DEVICE             = -15,
  ERR_MISMATCH              = -16,
  ERR_NOT_FOUND             = -17,
  ERR_CHECKSUM_ERROR        = -18,
  ERR_MALFORMED             = -19,
  ERR_INVALID_RPC_ID        = -20,
  ERR_INVALID_RPC_ARGS      = -21,
  ERR_NOSPC                 = -22,
  ERR_INVALID_ARGS          = -23,
  ERR_INVALID_LENGTH        = -24,
  ERR_NOT_IDLE              = -25,
  ERR_BAD_CONFIG            = -26,
  ERR_FLASH_HW_ERROR        = -27,
  ERR_FLASH_TIMEOUT         = -28,
  ERR_NO_MEMORY             = -29,
  ERR_READ_PROTECTED        = -30,
  ERR_WRITE_PROTECTED       = -31,
  ERR_AGAIN                 = -32,
  ERR_NOT_CONNECTED         = -33,
  ERR_BAD_PKT_SIZE          = -34,
  ERR_EXIST                 = -35,
  ERR_CORRUPT               = -36,
  ERR_NOTDIR                = -37,
  ERR_ISDIR                 = -38,
  ERR_NOTEMPTY              = -39,
  ERR_BADF                  = -40,
  ERR_FBIG                  = -41,
  ERR_INVALID_PARAMETER     = -42,
  ERR_NOATTR                = -43,
  ERR_TOOLONG               = -44,
  ERR_IO                    = -45,
  ERR_FS                    = -46,
  ERR_DMA_FIFO              = -47,
  ERR_INTERRUPTED           = -48,
  ERR_QUEUE_FULL            = -49,
  ERR_NO_ROUTE              = -50,
} error_t;

const char *error_to_string(error_t e);
