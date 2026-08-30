#pragma once

struct vllp;

/**
 * Returns NULL if
 *   - Provided image is already running (unless force is set)
 *   - Upgrade was successful
 *
 * Returns an error string (compile time constant) if an error occured
 *
 * force: push and reboot into elfimage even if its build-id matches
 * what's already running.
 */

const char *vllp_ota(struct vllp *v, const char *elfimage, int force);
