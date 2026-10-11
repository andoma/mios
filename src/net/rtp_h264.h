#pragma once

#include <stddef.h>
#include <stdint.h>

#include <mios/error.h>
#include <mios/video_sink.h>

// H.264 over RTP (RFC 6184), packetization mode 1 (single NAL unit
// packets and FU-A fragments, no aggregation)

#define RTP_H264_PAYLOAD_TYPE 96
#define RTP_H264_CLOCK_RATE   90000

// Called once per RTP payload. `hdr` is the 0 or 2 byte FU-A
// indicator/header to put before `payload`. `marker` is set on the
// last packet of the access unit.
typedef error_t (rtp_h264_emit_t)(void *opaque,
                                  const uint8_t *hdr, size_t hdrlen,
                                  const uint8_t *payload, size_t len,
                                  int marker);

// Split one Annex-B access unit (NAL units separated by 00 00 01 or
// 00 00 00 01 start codes) into RTP payloads of at most `max_payload`
// bytes
error_t rtp_h264_packetize(const uint8_t *au, size_t len, size_t max_payload,
                           rtp_h264_emit_t *emit, void *opaque);

// RTP sender, implementing video_sink_t. Frames are copied into pbufs
// before frame() returns. It must not be called from the net thread:
// when the pbuf pool runs low it waits for it to drain, and drops the
// rest of the frame if it doesn't.
video_sink_t *rtp_h264_create(uint32_t dst_addr, uint16_t dst_port);
