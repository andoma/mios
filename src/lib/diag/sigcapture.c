#include <mios/sigcapture.h>

#include <stdint.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <usb/usb_desc.h>
#include <usb/usb.h>

#include <mios/task.h>

#include "irq.h"

typedef enum {
  STATE_RUN,
  STATE_READOUT,
} sigcapture_state_t;


typedef struct sc_usb_pkt_preamble {
  uint8_t pkt_type;
  uint8_t channels;
  uint16_t depth;
  uint32_t nominal_frequency;
  uint16_t trig_offset;
} sc_usb_pkt_preamble_t;


typedef struct sc_usb_pkt_channel {
  uint8_t pkt_type;
  uint8_t unit;
  char name[14];
  float scale;
} sc_usb_pkt_channel_t;


struct sigcapture {
  int16_t *storage;
  uint32_t wrptr;
  sigcapture_state_t state;
  size_t trig_countdown;

  uint8_t usb_iface_subtype;
  uint8_t columns_per_xfer;
  uint32_t softirq;
  struct usb_interface *usb_iface;

  size_t depth;
  size_t channels;

  uint32_t send_index;

  sc_usb_pkt_preamble_t pkt_preamble;
  int16_t pkt_data[32];

  uint8_t pkt_trailer;

  sc_usb_pkt_channel_t pkt_channels[0];
};


int16_t *
sigcapture_wrptr(sigcapture_t *sc)
{
  if(sc->state != STATE_RUN)
    return NULL;

  if(sc->trig_countdown) {
    sc->trig_countdown--;
    if(sc->trig_countdown == 0) {
      sc->state = STATE_READOUT;
      softirq_raise(sc->softirq);
    }
  }


  int idx = sc->wrptr & (sc->depth - 1);
  sc->wrptr++;
  return sc->storage + idx * sc->channels;
}


void
sigcapture_trig(sigcapture_t *sc, size_t leading_samples)
{
  if(leading_samples >= sc->depth)
    return;
  if(leading_samples == 0)
    leading_samples = sc->depth / 2;

  if(sc->state == STATE_READOUT || sc->trig_countdown)
    return;
  sc->pkt_preamble.trig_offset = leading_samples;
  sc->trig_countdown = sc->depth - leading_samples;
}


static void
sigcapture_send(sigcapture_t *sc)
{
  usb_ep_t *ue = &sc->usb_iface->ui_endpoints[0];
  const void *data;
  size_t len;
  if(unlikely(sc->send_index == 0)) {
    data = &sc->pkt_preamble;
    len = sizeof(sc->pkt_preamble);

    sc->send_index = 0x100;
  } else if(unlikely(sc->send_index == 1)) {
    data = &sc->pkt_trailer;
    len = sizeof(sc->pkt_trailer);

    sc->send_index = -1;
  } else if(unlikely(sc->send_index < 0x10000)) {
    const int ch = sc->send_index & 0xff;
    data = &sc->pkt_channels[ch];
    len = sizeof(sc_usb_pkt_channel_t);

    if(ch == sc->channels - 1) {
      sc->send_index = 0x10000;
    } else {
      sc->send_index++;
    }

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

    if(column >= sc->depth) {
      sc->send_index = 1;
    } else {
      sc->send_index += sc->columns_per_xfer;
    }

    data = &sc->pkt_data;
    len = sizeof(sc->pkt_data);
  }

  ue->ue_vtable->write(ue->ue_dev, ue, data, len);
}


static void
sigcapture_txco(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  sigcapture_t *sc = ue->ue_iface_aux;
  if(sc->send_index == -1) {
    sc->state = STATE_RUN;
    return;
  }
  sigcapture_send(sc);
}

static void
sigcapture_tx_reset(device_t *d, usb_ep_t *ue)
{
  sigcapture_t *sc = ue->ue_iface_aux;
  sc->state = STATE_RUN;
}


static void
sigcapture_irq(void *arg)
{
  sigcapture_t *sc = arg;

  int q = irq_forbid(IRQ_LEVEL_NET);

  usb_ep_t *ue = &sc->usb_iface->ui_endpoints[0]; // IN
  if(ue->ue_running) {
    sc->send_index = 0;
    sigcapture_send(sc);
  } else {
    // USB-interface is not active, just start over
    sc->state = STATE_RUN;
  }
  irq_permit(q);

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
                            sizeof(sc_usb_pkt_channel_t) * channels);
  sc->depth = depth;
  sc->channels = channels;
  sc->storage = storage;
  sc->columns_per_xfer = 32 / channels;

  sc->pkt_preamble.pkt_type = 0xff;
  sc->pkt_preamble.channels = channels;
  sc->pkt_preamble.depth = depth;
  sc->pkt_preamble.nominal_frequency = nominal_frequency;

  sc->pkt_trailer = 0xfe;

  sc->usb_iface_subtype = usb_iface_subtype;

  for(size_t i = 0; i < channels; i++) {
    sc_usb_pkt_channel_t *c = &sc->pkt_channels[i];
    c->pkt_type = i;
    c->unit = channel_descriptors[i].unit;
    strlcpy(c->name, channel_descriptors[i].name, sizeof(c->name));
    c->scale = channel_descriptors[i].scale;
  }

  sc->softirq = softirq_alloc(sigcapture_irq, sc);
  sc->usb_iface = usb_alloc_interface(q, sigcapture_gen_desc, sc, 1,
                                      "sigcapture");
  usb_init_endpoint(&sc->usb_iface->ui_endpoints[0],
                    sc, sigcapture_txco, sigcapture_tx_reset,
                    USB_ENDPOINT_BULK, 0x80, 0x1, 64);
  return sc;
}
