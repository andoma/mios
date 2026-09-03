#pragma once

struct block_iface;

// Point the "ota" VLLP/DSIG service at an upgrade partition (or NULL to
// disable). Used by the OTA test suite; in a normal host run no partition
// is set and an "ota" open returns ERR_NO_DEVICE.
void host_ota_set_partition(struct block_iface *partition);
