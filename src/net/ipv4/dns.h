#pragma once

#include <stdint.h>

#include <mios/error.h>

// Resolve a host name to an IPv4 address (network byte order) using the
// DNS servers learned over DHCP. Dotted quads are returned as is.
// Blocks for up to a few seconds, call from a thread (not the network
// thread). Answers are cached according to their TTL
error_t dns_resolve(const char *name, uint32_t *addr);
