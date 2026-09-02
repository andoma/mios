#pragma once

// Anonymous mmap backing the Mios heap. Pages are only committed on
// first touch, so this is an upper bound, not a cost.
#define HOST_HEAP_SIZE (64 * 1024 * 1024)

// Plenty of packet buffers, memory is not the constraint here
#define PBUF_DEFAULT_COUNT 256
