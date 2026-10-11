#pragma once

#include <mios/error.h>

struct stream;

// CSI-2 host + D-PHY receiver. The CSI feeds the DCMIPP only; frames are
// captured by configuring a DCMIPP pipe.

// Clock, reset and D-PHY bring-up. `mbps` is the per-lane bit rate the
// sensor is configured for (selects the D-PHY frequency range).
error_t stm32n6_csi_init(int lanes, int mbps);

// Forward data type `dt` (e.g. CSI_DT_RAW10) on virtual channel `vc`,
// unpacked according to `ft` (CSI_FT_BPPxx). Must be called with the
// virtual channel stopped.
void stm32n6_csi_config_vc(int vc, int dt, int ft);

error_t stm32n6_csi_start(int vc);

void stm32n6_csi_stop(int vc);

// Frame counter for `vc` (incremented on every end of frame)
unsigned int stm32n6_csi_frame_count(int vc);

void stm32n6_csi_print_status(struct stream *st);

void stm32n6_csi_clear_stats(void);
