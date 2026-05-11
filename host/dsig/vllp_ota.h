#pragma once

struct vllp;

/**
 * Returns NULL if
 *   - Provided image is already running
 *   - Upgrade was successful
 *
 * Returns an error string (compile time constant) if an error occured
 */

const char *vllp_ota(struct vllp *v, const char *elfimage);
