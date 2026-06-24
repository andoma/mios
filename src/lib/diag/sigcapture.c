#include <mios/sigcapture.h>

#include <stdint.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <usb/usb_desc.h>
#include <usb/usb.h>

#include <mios/task.h>

#include "irq.h"

#include "sigcapture_internal.h"


int16_t *
sigcapture_wrptr(sigcapture_t *sc)
{
  if(sc->state != SIGCAPTURE_STATE_RUN)
    return NULL;

  if(sc->decimation > 1) {
    if(++sc->decim_phase < sc->decimation)
      return NULL;  // skip this sample
    sc->decim_phase = 0;
  }

  if(sc->trig_countdown) {
    sc->trig_countdown--;
    if(sc->trig_countdown == 0) {
      sc->state = SIGCAPTURE_STATE_READOUT;
      softirq_raise(sc->softirq);
    }
  }

  uint16_t depth = sc->depth;
  uint16_t mask = depth - 1;

  int idx = sc->wrptr & mask;
  // Keep track of if we have wrapped with a single bit
  sc->wrptr = (sc->wrptr + 1) | (depth & sc->wrptr);
  return sc->storage + idx * sc->channels;
}


void
sigcapture_trig(sigcapture_t *sc, size_t leading_samples)
{
  if(leading_samples >= sc->depth)
    return;
  if(leading_samples == 0)
    leading_samples = sc->depth / 2;

  if(sc->state == SIGCAPTURE_STATE_READOUT || sc->trig_countdown)
    return;
  sc->pkt_preamble.trig_offset = leading_samples;
  sc->trig_countdown = sc->depth - leading_samples;
}


void
sigcapture_reset(sigcapture_t *sc)
{
  sc->state = SIGCAPTURE_STATE_RUN;
  sc->wrptr = 0;
}


void
sigcapture_set_decimation(sigcapture_t *sc, unsigned int n)
{
  if(n < 1)
    n = 1;
  sc->decimation = n;
  sc->decim_phase = 0;
  sc->pkt_preamble.nominal_frequency = sc->base_frequency / n;
}


// Transport-neutral readout state machine. Emits, in order: preamble (0xff),
// one descriptor per channel, the captured data columns, then the trailer
// (0xfe). After the trailer it hands the buffer back to the sampler and reports
// completion. The wire format is shared verbatim by the USB and VLLP
// transports.
int
sigcapture_readout_next(sigcapture_t *sc, const void **data, size_t *len)
{
  if(sc->send_index == 0) {

    if(sc->wrptr & sc->depth) {
      // Wrapped -- the whole ring is valid.
      sc->xfer_rows = sc->depth;
    } else {
      // Partially filled -- only [0, wrptr) is valid.
      sc->pkt_preamble.trig_offset -= (sc->depth - sc->wrptr);
      sc->xfer_rows = sc->wrptr;
      sc->wrptr = 0;
    }
    sc->pkt_preamble.depth = sc->xfer_rows;

    *data = &sc->pkt_preamble;
    *len = sizeof(sc->pkt_preamble);

    sc->send_index = 0x100;
    return 1;

  } else if(sc->send_index == (uint32_t)-1) {
    // Trailer was already emitted; readout complete.
    sigcapture_reset(sc);
    return 0;

  } else if(sc->send_index == 1) {
    *data = &sc->pkt_trailer;
    *len = sizeof(sc->pkt_trailer);

    sc->send_index = -1;
    return 1;

  } else if(sc->send_index < 0x10000) {
    const int ch = sc->send_index & 0xff;
    *data = &sc->pkt_channels[ch];
    *len = sizeof(sc_pkt_channel_t);

    if(ch == sc->channels - 1) {
      sc->send_index = 0x10000;
    } else {
      sc->send_index++;
    }
    return 1;

  } else {
    uint32_t column = sc->send_index & 0xffff;

    int16_t *dst = sc->pkt_data;
    for(size_t i = 0; i < sc->columns_per_xfer; i++) {
      int idx = (column + sc->wrptr) & (sc->depth - 1);
      const int16_t *col = sc->storage + (idx * sc->channels);
      for(size_t j = 0; j < sc->channels; j++) {
        *dst++ = *col++;
      }
      column++;
    }

    if(column >= sc->xfer_rows) {
      sc->send_index = 1;
    } else {
      sc->send_index += sc->columns_per_xfer;
    }

    *data = &sc->pkt_data;
    *len = sizeof(sc->pkt_data);
    return 1;
  }
}


static void
sigcapture_softirq(void *arg)
{
  sigcapture_t *sc = arg;
  if(sc->on_complete)
    sc->on_complete(sc->on_complete_opaque);
}


sigcapture_t *
sigcapture_alloc(size_t depth_power_of_2, size_t channels,
                 const sigcapture_desc_t channel_descriptors[],
                 uint32_t nominal_frequency)
{
  if(depth_power_of_2 < 5)
    return NULL; // Too short, makes no sense

  const size_t depth = 1 << depth_power_of_2;

  if(channels > 16)
    return NULL;

  int16_t *storage = xalloc(depth * channels * sizeof(int16_t), 0,
                            MEM_MAY_FAIL);
  if(storage == NULL)
    return NULL;

  sigcapture_t *sc = calloc(1, sizeof(sigcapture_t) +
                            sizeof(sc_pkt_channel_t) * channels);
  sc->depth = depth;
  sc->channels = channels;
  sc->storage = storage;
  sc->columns_per_xfer = 32 / channels;
  sc->decimation = 1;
  sc->base_frequency = nominal_frequency;

  sc->pkt_preamble.pkt_type = 0xff;
  sc->pkt_preamble.channels = channels;
  sc->pkt_preamble.depth = depth;
  sc->pkt_preamble.nominal_frequency = nominal_frequency;

  sc->pkt_trailer = 0xfe;

  for(size_t i = 0; i < channels; i++) {
    sc_pkt_channel_t *c = &sc->pkt_channels[i];
    c->pkt_type = i;
    c->unit = channel_descriptors[i].unit;
    strlcpy(c->name, channel_descriptors[i].name, sizeof(c->name));
    c->scale = channel_descriptors[i].scale;
  }

  sc->softirq = softirq_alloc(sigcapture_softirq, sc);
  return sc;
}


// --- USB transport ----------------------------------------------------------

static void
sigcapture_usb_send(sigcapture_t *sc)
{
  const void *data;
  size_t len;
  if(!sigcapture_readout_next(sc, &data, &len))
    return; // Readout complete (buffer already handed back to the sampler).

  usb_ep_t *ue = &sc->usb_iface->ui_endpoints[0];
  ue->ue_vtable->write(ue->ue_dev, ue, data, len);
}


// Completion hook for the USB transport. Runs from the sigcapture softirq.
static void
sigcapture_usb_kick(void *opaque)
{
  sigcapture_t *sc = opaque;

  int q = irq_forbid(IRQ_LEVEL_NET);

  usb_ep_t *ue = &sc->usb_iface->ui_endpoints[0]; // IN
  if(ue->ue_running) {
    sc->send_index = 0;
    sigcapture_usb_send(sc);
  } else {
    // USB-interface is not active, just start over
    sigcapture_reset(sc);
  }
  irq_permit(q);
}


static void
sigcapture_txco(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  sigcapture_t *sc = ue->ue_iface_aux;
  // When the readout is complete, sigcapture_usb_send() resets and writes
  // nothing, ending the transmit chain.
  sigcapture_usb_send(sc);
}

static void
sigcapture_tx_reset(device_t *d, usb_ep_t *ue)
{
  sigcapture_t *sc = ue->ue_iface_aux;
  sigcapture_reset(sc);
}


static size_t
sigcapture_gen_desc(void *ptr, void *opaque, int iface_index)
{
  sigcapture_t *sc = opaque;
  return usb_gen_iface_desc(ptr, iface_index, 1, 255, sc->usb_iface_subtype);
}

sigcapture_t *
sigcapture_create(size_t depth_power_of_2, size_t channels,
                  const sigcapture_desc_t channel_descriptors[],
                  uint32_t nominal_frequency,
                  struct usb_interface_queue *q,
                  uint8_t usb_iface_subtype)
{
  sigcapture_t *sc = sigcapture_alloc(depth_power_of_2, channels,
                                      channel_descriptors, nominal_frequency);
  if(sc == NULL)
    return NULL;

  sc->usb_iface_subtype = usb_iface_subtype;
  sc->on_complete = sigcapture_usb_kick;
  sc->on_complete_opaque = sc;

  sc->usb_iface = usb_alloc_interface(q, sigcapture_gen_desc, sc, 1,
                                      "sigcapture");
  usb_init_endpoint(&sc->usb_iface->ui_endpoints[0],
                    sc, sigcapture_txco, sigcapture_tx_reset,
                    USB_ENDPOINT_BULK, 0x80, 0x1, 64);
  return sc;
}
