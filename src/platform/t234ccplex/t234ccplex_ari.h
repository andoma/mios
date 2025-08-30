#pragma once

#include <stdint.h>

#include <mios/error.h>

#define TEGRA_ARI_VERSION_CMD    0
#define TEGRA_ARI_ECHO_CMD       1
#define TEGRA_ARI_NUM_CORES_CMD  2

error_t ari_cmd(uint32_t cmd, uint64_t in, uint64_t *out);

