#pragma once

#include <mios/sigcapture.h>

#include <stddef.h>
#include <stdint.h>

// Internal interface shared between the sigcapture core (sigcapture.c) and
// its transports (USB in sigcapture.c, VLLP in sigcapture_vllp.c). Not a
// public API.

typedef enum {
  SIGCAPTURE_STATE_RUN,      // sigcapture_wrptr() is filling the ring buffer
  SIGCAPTURE_STATE_READOUT,  // buffer frozen, a transport is draining it
} sigcapture_state_t;

// Wire frames. Must match the host parser (host/mcp/mcp_tool_sigcapture.c)
// and are identical across the USB and VLLP transports.
typedef struct sc_pkt_preamble {
  uint8_t pkt_type;  // 0xff
  uint8_t channels;
  uint16_t depth;
  uint32_t nominal_frequency;
  int16_t trig_offset;
} sc_pkt_preamble_t;

typedef struct sc_pkt_channel {
  uint8_t pkt_type;  // channel index
  uint8_t unit;
  char name[14];
  float scale;
} sc_pkt_channel_t;

struct usb_interface;

struct sigcapture {
  int16_t *storage;
  uint16_t wrptr;
  uint16_t depth;
  sigcapture_state_t state;
  size_t trig_countdown;

  uint32_t softirq;

  size_t channels;
  uint8_t columns_per_xfer;

  // Decimation: store 1 of every `decimation` samples (>=1), to capture slow
  // phenomena over a longer window. base_frequency is the full-rate frequency;
  // the preamble reports base_frequency / decimation.
  uint16_t decimation;
  uint16_t decim_phase;
  uint32_t base_frequency;

  // Readout cursor + frame staging (only touched during READOUT).
  uint32_t send_index;
  uint32_t xfer_rows;
  sc_pkt_preamble_t pkt_preamble;
  int16_t pkt_data[32];
  uint8_t pkt_trailer;

  // Transport-agnostic completion hook, invoked from the sigcapture softirq
  // (dispatched at PendSV / IRQ_LEVEL_SWITCH) when a capture completes.
  void (*on_complete)(void *opaque);
  void *on_complete_opaque;

  // USB transport (NULL when the instance is VLLP-only).
  uint8_t usb_iface_subtype;
  struct usb_interface *usb_iface;

  sc_pkt_channel_t pkt_channels[0];
};

// Allocate the ring buffer + channel descriptors and the completion softirq.
// No transport is attached; the caller wires on_complete. Returns NULL on bad
// arguments or out of memory.
sigcapture_t *sigcapture_alloc(size_t depth_power_of_2, size_t channels,
                               const sigcapture_desc_t descs[],
                               uint32_t nominal_frequency);

// Produce the next readout frame into (*data, *len). Returns 1 while frames
// remain, or 0 once the capture is fully emitted -- at which point the buffer
// has been handed back to the sampler via sigcapture_reset(). Only call while
// state == SIGCAPTURE_STATE_READOUT.
int sigcapture_readout_next(sigcapture_t *sc, const void **data, size_t *len);

// Hand the buffer back to the sampler (state -> RUN, wrptr -> 0).
void sigcapture_reset(sigcapture_t *sc);
