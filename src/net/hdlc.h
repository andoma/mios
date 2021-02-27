#pragma once

#include <sys/uio.h>
#include <stddef.h>
#include <stdint.h>

// Primary interface (ie not the raw -interface) adds and checks CRC32

// Keeps reading from s and calls cb with deframed packets
// This function never returns (call it from a thread)
void hdlc_read(stream_t *s,
               void (*cb)(void *opaque, const uint8_t *data, size_t len),
               void *opaque, size_t max_frame_size);

void hdlc_send(stream_t *s, const void *data, size_t len);


// Raw send (does not apply CRC)
void hdlc_write_rawv(stream_t *s, struct iovec *iov, size_t count);
