#pragma once

#define FDCAN_ENABLE_TDC        0x1

// External loopback test mode: the core receives and ACKs its own
// transmission internally while still driving the TX pin; the RX pin
// is ignored. For bring-up without a transceiver.
#define FDCAN_ENABLE_LOOPBACK   0x2
