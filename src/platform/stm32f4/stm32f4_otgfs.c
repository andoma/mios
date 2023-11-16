#include <stdlib.h>
#include <sys/param.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>

#include <mios/io.h>
#include <mios/device.h>
#include <mios/task.h>
#include <mios/timer.h>

#include "irq.h"

#include "stm32f4_reg.h"
#include "stm32f4_clk.h"
#include "stm32f4_usb.h"

#include <usb/usb.h>
#include <usb/usb_desc.h>

#define MAX_NUM_ENDPOINTS 6

#define OTG_FS_BASE 0x50000000

#define OTG_FS_GOTGCTL  (OTG_FS_BASE + 0x000)
#define OTG_FS_GAHBCFG  (OTG_FS_BASE + 0x008)
#define OTG_FS_GUSBCFG  (OTG_FS_BASE + 0x00c)
#define OTG_FS_GRSTCTL  (OTG_FS_BASE + 0x010)
#define OTG_FS_GINTSTS  (OTG_FS_BASE + 0x014)
#define OTG_FS_GINTMSK  (OTG_FS_BASE + 0x018)
#define OTG_FS_GRXSTSP  (OTG_FS_BASE + 0x020)
#define OTG_FS_GRXFSIZ  (OTG_FS_BASE + 0x024)
#define OTG_FS_NPTXFSIZ (OTG_FS_BASE + 0x028)
#define OTG_FS_GCCFG    (OTG_FS_BASE + 0x038)

#define OTG_FS_DIEPTXF(x) (OTG_FS_BASE + 0x100 + 0x4 * (x))

#define OTG_FS_DCFG     (OTG_FS_BASE + 0x800)
#define OTG_FS_DCTL     (OTG_FS_BASE + 0x804)
#define OTG_FS_DSTS     (OTG_FS_BASE + 0x808)
#define OTG_FS_DIEPMSK  (OTG_FS_BASE + 0x810)
#define OTG_FS_DOEPMSK  (OTG_FS_BASE + 0x814)
#define OTG_FS_DAINT    (OTG_FS_BASE + 0x818)
#define OTG_FS_DAINTMSK (OTG_FS_BASE + 0x81c)

#define OTG_FS_PCGCCTL  (OTG_FS_BASE + 0xe00)

#define OTG_FS_DIEPCTL(x)  (OTG_FS_BASE + 0x900 + 0x20 * (x))
#define OTG_FS_DIEPINT(x)  (OTG_FS_BASE + 0x908 + 0x20 * (x))
#define OTG_FS_DIEPTSIZ(x) (OTG_FS_BASE + 0x910 + 0x20 * (x))
#define OTG_FS_DTXFSTS(x)  (OTG_FS_BASE + 0x918 + 0x20 * (x))

#define OTG_FS_DOEPCTL(x)  (OTG_FS_BASE + 0xb00 + 0x20 * (x))
#define OTG_FS_DOEPINT(x)  (OTG_FS_BASE + 0xb08 + 0x20 * (x))
#define OTG_FS_DOEPTSIZ(x) (OTG_FS_BASE + 0xb10 + 0x20 * (x))

#define OTG_FS_FIFO(x)  (OTG_FS_BASE + 0x1000 + (x) * 0x1000)

#define OTG_FS_GINT_OEPINT   (1 << 19)
#define OTG_FS_GINT_IEPINT   (1 << 18)
#define OTG_FS_GINT_USBRST   (1 << 12)
#define OTG_FS_GINT_ENUMDNE  (1 << 13)
#define OTG_FS_GINT_USBSUSP  (1 << 11)
#define OTG_FS_GINT_ESUSP    (1 << 10)
#define OTG_FS_GINT_NPTXFE   (1 << 5)
#define OTG_FS_GINT_RXFLVL   (1 << 4)
#define OTG_FS_GINT_SOF      (1 << 3)

#define OTG_FS_DOEP_STUP     (1 << 3)
#define OTG_FS_DOEP_XFRC     (1 << 0)

#define OTG_FS_DIEP_TOC      (1 << 3)
#define OTG_FS_DIEP_XFRC     (1 << 0)




typedef struct tx_ptr {
  const void *ptr;
  uint8_t (*getch)(void *opaque);
  uint16_t offset;
  uint16_t total;
  uint8_t zlp;
} tx_ptr_t;


typedef struct ep0_in {

  usb_ep_t ue;

  tx_ptr_t tx_ptr;

} ep0_in_t;



typedef struct ep0_out {
  usb_ep_t ue;

  union {
    uint32_t setup_words[2];
    uint8_t setup_u8[8];

    struct usb_setup_packet setup_pkt;
  };
} ep0_out_t;



struct usb_ctrl {

  device_t uc_dev;

  uint8_t uc_num_endpoints;
  uint8_t uc_assigned_addr;
  struct usb_device_descriptor uc_desc;

  usb_ep_t *uc_in_ue[MAX_NUM_ENDPOINTS];
  usb_ep_t *uc_out_ue[MAX_NUM_ENDPOINTS];

  const char *uc_manufacturer;
  const char *uc_product;

  ep0_in_t uc_ep0_in;   // TX
  ep0_out_t uc_ep0_out; // RX

  uint8_t *uc_config_desc;
  size_t uc_config_desc_size;

  task_t uc_softirq;
  timer_t uc_reconnect;

  uint32_t uc_resets;
  uint32_t uc_enumerations;
  uint32_t uc_erratic_errors;

  struct usb_interface_queue uc_ifaces;
};



typedef struct usb_ctrl usb_ctrl_t;



static void
ep_start(int ep, size_t len)
{
  reg_wr(OTG_FS_DIEPTSIZ(ep),
         (1 << 19) |
         len);
  reg_or(OTG_FS_DIEPCTL(ep),
         (1 << 31) |
         (1 << 26));
}


static void
send_zlp(int ep)
{
  ep_start(ep, 0);
}


static void
fifo_write_fn(int ep, uint8_t (*getch)(void *opaque),
              void *opaque, size_t len)
{
  ep_start(ep, len);

  uint32_t u32 = 0;
  for(size_t i = 0; i < len; i++) {
    uint8_t b = getch(opaque);
    u32 |= b << ((i & 3) * 8);

    if((i & 3) == 3) {
      reg_wr(OTG_FS_FIFO(ep), u32);
      u32 = 0;
    }
  }

  if(len & 3) {
    reg_wr(OTG_FS_FIFO(ep), u32);
  }
}


uint8_t
read_tx_ptr(void *opaque)
{
  tx_ptr_t *tp = opaque;
  const uint8_t *data = tp->ptr;
  return data[tp->offset++];
}


uint8_t
read_string_as_desc(void *opaque)
{
  tx_ptr_t *tp = opaque;
  int chof;
  uint8_t r;
  const uint8_t *data = tp->ptr;

  switch(tp->offset) {
  case 0:
    r = tp->total;
    break;
  case 1:
    r = USB_DESC_TYPE_STRING;
    break;
  default:
    chof = (tp->offset - 2) / 2;
    if(tp->offset & 1) {
      r = 0;
    } else {
      r = data[chof];
    }
    break;
  }
  tp->offset++;
  return r;
}


uint8_t
read_bin2hex_as_desc(void *opaque)
{
  tx_ptr_t *tp = opaque;
  int chof;
  uint8_t r;
  const uint8_t *data = tp->ptr;

  switch(tp->offset) {
  case 0:
    r = tp->total;
    break;
  case 1:
    r = USB_DESC_TYPE_STRING;
    break;
  default:
    chof = (tp->offset - 2) / 2;
    if(tp->offset & 1) {
      r = 0;
    } else {
      const uint8_t byte_idx = chof / 2;
      const uint8_t b = data[byte_idx] >> (chof & 1 ? 0 : 4);
      r = "0123456789abcdef"[b & 0xf];
    }
    break;
  }
  tp->offset++;
  return r;
}



static void
tx_start(int ep, tx_ptr_t *tx, int max_packet_size)
{
  if(ep != 0)
    while(reg_rd(OTG_FS_DIEPCTL(ep)) & (1 << 31)) {}

  const size_t avail_words = reg_rd(OTG_FS_DTXFSTS(ep));
  if(avail_words == 0) {
    panic("tx_start but fifo full");
  }
  const size_t avail_bytes = avail_words * 4;
  const size_t remain = tx->total - tx->offset;
  const size_t len = MIN(MIN(avail_bytes, remain), max_packet_size);

  fifo_write_fn(ep, tx->getch, &tx->ptr, len);
}


static error_t
stm32f4_ep_write(device_t *dev, usb_ep_t *ue,
                 const uint8_t *buf, size_t len)
{
  const uint32_t ep = ue->ue_address & 0x7f;

  if(reg_rd(OTG_FS_DIEPCTL(ep)) & (1 << 31)) {
    ue->ue_num_drops++;
    return ERR_NOT_READY;
  }

  const int avail_words = reg_rd(OTG_FS_DTXFSTS(ep));
  const int avail_bytes = avail_words * 4;

  if(len + 4 > avail_bytes) {
    ue->ue_num_drops++;
    return ERR_NOT_READY;
  }

  ue->ue_num_packets++;

  ep_start(ep, len);

  size_t i = 0;
  if(((intptr_t)buf & 0x3) == 0) {
    for(; i + 3 < len; i += 4) {
      reg_wr(OTG_FS_FIFO(ep), *(uint32_t *)&buf[i]);
    }
  }

  uint32_t u32 = 0;
  for(; i < len; i++) {
    uint8_t b = buf[i];
    u32 |= b << ((i & 3) * 8);

    if((i & 3) == 3) {
      reg_wr(OTG_FS_FIFO(ep), u32);
      u32 = 0;
    }
  }

  if(len & 3) {
    reg_wr(OTG_FS_FIFO(ep), u32);
  }
  return 0;
}




static void
drop_packet(int ep, int bcnt)
{
  int words = (bcnt + 3) / 4;

  for(int i = 0; i < words; i++) {
    reg_rd(OTG_FS_FIFO(ep));
  }
}


static void
ep_cnak(int ep)
{
  reg_wr(OTG_FS_DOEPCTL(ep),
         reg_rd(OTG_FS_DOEPCTL(ep)) |
         (1 << 26));
}


static void
set_address(uint8_t addr)
{
  reg_set_bits(OTG_FS_DCFG, 4, 7, addr);
}


static void
handle_get_descriptor(usb_ctrl_t *uc)
{
  const uint8_t type = uc->uc_ep0_out.setup_pkt.value >> 8;
  const uint8_t index = uc->uc_ep0_out.setup_pkt.value;

  const void *desc = NULL;
  size_t desclen = 0;

  uint8_t (*getch)(void *opaque) = read_tx_ptr;

  switch(type) {

  case USB_DESC_TYPE_DEVICE:
    if(index != 0)
      break;
    desc = &uc->uc_desc;
    desclen = sizeof(struct usb_device_descriptor);
    break;

  case USB_DESC_TYPE_CONFIGURATION:
    if(index != 0)
      break;
    desc = uc->uc_config_desc;
    desclen = uc->uc_config_desc_size;
    break;

  case USB_DESC_TYPE_STRING:

    getch = read_string_as_desc;
    switch(index) {
    case 0:
      desc = "\x09\x04";
      desclen = 2;
      break;
    case 1:
      desc = uc->uc_manufacturer;
      desclen = strlen(desc);
      break;
    case 2:
      desc = uc->uc_product;
      desclen = strlen(desc);
      break;
    case 3:
      getch = read_bin2hex_as_desc;
      desc = (uint8_t *)0x1fff7a10; // UUID stm32f4
      desclen = 24;
      break;
    }
    desclen = desclen * 2 + 2;
    break;

  default:
    break;
  }

  if(desc == NULL) {
    send_zlp(0);
    return;
  }

  const int reqlen = uc->uc_ep0_out.setup_pkt.length;

  tx_ptr_t *tp = &uc->uc_ep0_in.tx_ptr;

  tp->ptr = desc;
  tp->getch = getch;
  tp->offset = 0;
  tp->total = MIN(reqlen, desclen);
  tp->zlp = 0;

  // If the descriptor is shorter then asked for and is a multiple of 8
  // we must add a trailing ZLP
  if(desclen < reqlen && !(desclen & 7))
    tp->zlp = 1;

  tx_start(0, tp, 8);
}


static void
ep0_handle_set_config(usb_ctrl_t *uc)
{
  for(int ep = 1; ep < uc->uc_num_endpoints; ep++) {

    if(uc->uc_in_ue[ep] != NULL) {
      usb_ep_t *in = uc->uc_in_ue[ep];
      in->ue_running = 1;
      reg_or(OTG_FS_DAINTMSK,
             (1 << ep));

      reg_wr(OTG_FS_DIEPCTL(ep),
             (ep << 22) | // FIFO
             (in->ue_endpoint_type << 18) | // Type
             (1 << 15) |  // USB ACTIVE ENDPOINT
             in->ue_max_packet_size);

      if(in->ue_reset != NULL)
        in->ue_reset(&uc->uc_dev, in);

    }


    if(uc->uc_out_ue[ep] != NULL) {
      usb_ep_t *out = uc->uc_out_ue[ep];
      out->ue_running = 1;

      reg_or(OTG_FS_DAINTMSK,
             (0x10000 << ep));

      reg_wr(OTG_FS_DOEPTSIZ(ep),
             (1 << 29) |
             (1 << 19) |
             out->ue_max_packet_size);

      reg_wr(OTG_FS_DOEPCTL(ep),
             (1 << 31) |
             (1 << 26) |
             (out->ue_endpoint_type << 18) | // Type
             (1 << 15) |  // USB ACTIVE ENDPOINT
             out->ue_max_packet_size);

      if(out->ue_reset != NULL)
        out->ue_reset(&uc->uc_dev, out);

    }
  }
}


static void
ep0_handle_setup(usb_ctrl_t *uc)
{
  const struct usb_setup_packet *usp = &uc->uc_ep0_out.setup_pkt;
  int recipient = usp->request_type & 0x1f;
  int type = (usp->request_type >> 5) & 3;

  if(type == 0 && recipient == 0) {

    switch(uc->uc_ep0_out.setup_pkt.request) {

    case USB_REQ_GET_STATUS:
      send_zlp(0);
      break;

    case USB_REQ_SET_ADDRESS:
      uc->uc_assigned_addr = uc->uc_ep0_out.setup_pkt.value;
      set_address(uc->uc_ep0_out.setup_pkt.value);
      send_zlp(0);
      break;

    case USB_REQ_GET_DESCRIPTOR:
      handle_get_descriptor(uc);
      break;

    case USB_REQ_SET_CONFIG:
      ep0_handle_set_config(uc);
      send_zlp(0);
      break;

    default:
      panic("Can't handle setup %d",
            uc->uc_ep0_out.setup_pkt.request);
    }
    return;
  }


  if(type == 1 && recipient == 1) {

    usb_interface_t *ui;
    STAILQ_FOREACH(ui, &uc->uc_ifaces, ui_link) {
      if(ui->ui_index == usp->index && ui->ui_iface_cfg) {
        ui->ui_iface_cfg(ui->ui_opaque, usp->request, usp->value);
      }
    }
  }
  send_zlp(0);
}





static void
ep0_rx(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)d;
  int ep = 0;

  if(flags & USB_COMPLETED_IS_SETUP) {
    uc->uc_ep0_out.setup_words[0] = reg_rd(OTG_FS_FIFO(0));
    uc->uc_ep0_out.setup_words[1] = reg_rd(OTG_FS_FIFO(0));
    ep0_handle_setup(uc);
  } else {
    drop_packet(ep, bytes);
  }
  ep_cnak(ep);
}




static void
ep0_txco(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)d;

  tx_ptr_t *tp = &uc->uc_ep0_in.tx_ptr;
  if(tp->ptr == NULL)
    return;

  if(tp->offset != tp->total) {
    tx_start(0, tp, 8);
  } else if(tp->zlp) {
    tp->zlp = 0;
    send_zlp(0);
  } else {
    tp->ptr = NULL;
  }
}



static void
ep0_reset(device_t *d, usb_ep_t *ue)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)d;
  tx_ptr_t *tp = &uc->uc_ep0_in.tx_ptr;
  tp->ptr = NULL;
}



// In Endpoint Interrupt
static void
handle_iepint(usb_ctrl_t *uc)
{
  uint32_t daint = reg_rd(OTG_FS_DAINT);

  for(int ep = 0; ep < uc->uc_num_endpoints; ep++) {
    if(!((1 << ep) & daint)) // replace with ffs()
      continue;

    uint32_t diepint = reg_rd(OTG_FS_DIEPINT(ep));
    reg_wr(OTG_FS_DIEPINT(ep), diepint);

    usb_ep_t *ue = uc->uc_in_ue[ep];
    if(ue == NULL)
      continue;

    if(diepint & 1) {
      if(ue->ue_completed)
        ue->ue_completed(&uc->uc_dev, ue, 0, 0);
    }
  }
}


// Out Endpoint Interrupt
static void
handle_oepint(usb_ctrl_t *uc)
{
  uint32_t daint = reg_rd(OTG_FS_DAINT);

  for(int i = 0; i < uc->uc_num_endpoints; i++) {
    if(!((0x10000 << i) & daint))
      continue;

    uint32_t doepint = reg_rd(OTG_FS_DOEPINT(i));
    reg_wr(OTG_FS_DOEPINT(i), doepint);
    panic("ep%d OUT INT:0x%x\n", i, doepint);
  }
}




static void
handle_reset(usb_ctrl_t *uc)
{
  set_address(0);

  for(int ep = 0; ep < uc->uc_num_endpoints; ep++) {
    usb_ep_t *ue;

    // Turn off active endpoint bits
    reg_clr_bit(OTG_FS_DIEPCTL(ep), 15);
    reg_clr_bit(OTG_FS_DOEPCTL(ep), 15);

    // Clear all pending IRQs
    reg_wr(OTG_FS_DIEPINT(ep), 0xffffffff);
    reg_wr(OTG_FS_DOEPINT(ep), 0xffffffff);

    // Set SNAK bit
    reg_wr(OTG_FS_DOEPCTL(ep), (1 << 27));

    ue = uc->uc_in_ue[ep];
    if(ue != NULL) {
      ue->ue_running = 0;
      if(ue->ue_reset != NULL)
        ue->ue_reset(&uc->uc_dev, ue);
    }

    ue = uc->uc_out_ue[ep];
    if(ue != NULL) {
      ue->ue_running = 0;
      if(ue->ue_reset != NULL)
        ue->ue_reset(&uc->uc_dev, ue);
    }
  }

  reg_wr(OTG_FS_DOEPMSK, 0);

  reg_wr(OTG_FS_DIEPMSK,
         OTG_FS_DIEP_XFRC);

  // Enable interrupts for EP0
  reg_wr(OTG_FS_DAINTMSK, 0x00010001);

  // Setup EP0 for 8 byte transfers
  reg_wr(OTG_FS_DOEPTSIZ(0),
         (1 << 29) |
         (1 << 19) |
         8);

  reg_wr(OTG_FS_DOEPCTL(0),
         (1 << 26) |
         (1 << 31) | 3);
}


static void
handle_enum_done(usb_ctrl_t *uc)
{
  // Setup EP0 for 8 byte transfers
  reg_wr(OTG_FS_DIEPCTL(0),
         (1 << 27) |
         3);
}







static usb_ctrl_t g_usb_ctrl = {
  .uc_desc = {
    .bLength            = sizeof(struct usb_device_descriptor),
    .bDescriptorType    = USB_DESC_TYPE_DEVICE,
    .bcdUSB             = 0x200,
    .bDeviceClass       = 0xef,
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = 8,
    .bcdDevice          = 0x100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
  },

  .uc_in_ue[0] = &g_usb_ctrl.uc_ep0_in.ue,
  .uc_out_ue[0] = &g_usb_ctrl.uc_ep0_out.ue,

  .uc_ep0_in = {
    .ue = {
      .ue_iface_aux = &g_usb_ctrl.uc_ep0_in,
      .ue_completed = ep0_txco,
      .ue_reset = ep0_reset,
      .ue_name = "ep0",
    }
  },

  .uc_ep0_out = {
    .ue = {
      .ue_iface_aux = &g_usb_ctrl.uc_ep0_out,
      .ue_completed = ep0_rx,
      .ue_reset = ep0_reset,
      .ue_name = "ep0",
    }
  }

};





void
irq_67(void)
{
  usb_ctrl_t *uc = &g_usb_ctrl;

  while(1) {

    uint32_t gint = reg_rd(OTG_FS_GINTSTS) & reg_rd(OTG_FS_GINTMSK);
    if(gint == 0)
      break;

    if(gint & OTG_FS_GINT_USBRST) {
      handle_reset(uc);
      reg_wr(OTG_FS_GINTSTS, OTG_FS_GINT_USBRST);
      uc->uc_resets++;
    }

    if(gint & OTG_FS_GINT_ENUMDNE) {
      handle_enum_done(uc);
      reg_wr(OTG_FS_GINTSTS, OTG_FS_GINT_ENUMDNE);
      uc->uc_enumerations++;
    }

    // Read from FIFO
    if(gint & OTG_FS_GINT_RXFLVL) {
      const uint32_t rspr = reg_rd(OTG_FS_GRXSTSP);
      const uint32_t ep = rspr & 0xf;
      if(ep < MAX_NUM_ENDPOINTS) {
        usb_ep_t *ue = uc->uc_out_ue[ep];
        if(ue == NULL) {
          // Got packet on uninitialized endpoint, ignore
          if(ep < uc->uc_num_endpoints)
            ep_cnak(ep);
        } else {
          const uint32_t status = (rspr >> 17) & 0xf;
          const uint32_t bytes = (rspr >> 4) & 0x7ff;
          switch(status) {
          case 2:
            ue->ue_completed(&uc->uc_dev, ue, bytes, 0);
            break;
          case 6:
            ue->ue_completed(&uc->uc_dev, ue, bytes, USB_COMPLETED_IS_SETUP);
            break;
          }
        }
      }
    }

    if(gint & OTG_FS_GINT_OEPINT) {
      handle_oepint(uc);
    }

    if(gint & OTG_FS_GINT_IEPINT) {
      handle_iepint(uc);
    }


    if(gint & OTG_FS_GINT_ESUSP) {
      // Early suspend
      reg_wr(OTG_FS_GINTSTS, OTG_FS_GINT_ESUSP);

      if(reg_rd(OTG_FS_DSTS) & 8) {
        // Erratic error, trig reset
        task_run(&g_usb_ctrl.uc_softirq);
        uc->uc_erratic_errors++;
      }
    }
  }
}


static void  __attribute__((destructor(900)))
usb_fini(void)
{
  reg_set_bits(OTG_FS_DCTL, 1, 1, 1); // Soft disconnect
}

static void
stm32f4_otgfs_init_regs(usb_ctrl_t *uc)
{
  reg_wr(OTG_FS_GINTMSK, 0);          // Disable all interrupts
  reg_wr(OTG_FS_DIEPMSK, 0);
  reg_wr(OTG_FS_DOEPMSK, 0);
  reg_wr(OTG_FS_GINTSTS, 0xffffffff); // Clear interrupts

  reg_wr(OTG_FS_GAHBCFG, 1); // Enable global interrupts

  reg_wr(OTG_FS_GUSBCFG,
         (1 << 30) | // Force device mode
         (6 << 10) | // USB turnaroundtime = 6 (AHB > 32MHz)
         (1 << 6)  | // PHYSEL = 1 (full-speed)
         0);

  reg_wr(OTG_FS_GCCFG,
         (1 << 21) | // Disable VBUS sense
         0);

  reg_set_bits(OTG_FS_GOTGCTL, 6, 2, 3);

  reg_wr(OTG_FS_PCGCCTL, 0);   // Make sure clocks are not gated

  reg_wr(OTG_FS_DCTL,
         (1 << 1) | // Soft disconnect
         0);

  reg_wr(OTG_FS_DCFG,
         (reg_rd(OTG_FS_DCFG) & 0xffff0000) |
         (3 << 0) | // Device speed = 0b11 (Full speed)
         0);

  int fifo_words = 64;
  int fifo_addr = 0;
  reg_wr(OTG_FS_GRXFSIZ, fifo_words);
  fifo_addr += fifo_words;

  fifo_words = 32;
  reg_wr(OTG_FS_NPTXFSIZ,
         (fifo_words << 16) |
         fifo_addr);

  fifo_addr += fifo_words;

  for(int i = 1; i < uc->uc_num_endpoints; i++) {
    reg_wr(OTG_FS_DIEPTXF(i),
         (fifo_words << 16) |
         fifo_addr);
    fifo_addr += fifo_words;
  }

  reg_wr(OTG_FS_GINTMSK,
         OTG_FS_GINT_USBRST |
         OTG_FS_GINT_ENUMDNE |
         OTG_FS_GINT_RXFLVL |
         OTG_FS_GINT_IEPINT |
         OTG_FS_GINT_OEPINT |
         OTG_FS_GINT_OEPINT |
         OTG_FS_GINT_ESUSP |
         0);

  // Power up
  reg_set_bits(OTG_FS_GCCFG, 16, 1, 1);

  // Disconnect for 5ms
  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  timer_arm_abs(&uc->uc_reconnect, clock_get_irq_blocked() + 500000);
  irq_permit(q);
}


static void
stm32f4_ep_read(device_t *dev, usb_ep_t *ue,
                uint8_t *buf, size_t buf_size,
                size_t buf_offset, size_t bytes)
{
  const uint32_t ep = ue->ue_address & 0x7f;

  uint32_t w = 0;
  for(size_t i = 0; i < bytes; i++) {
    if((i & 3) == 0)
      w = reg_rd(OTG_FS_FIFO(ep));

    buf[(buf_offset + i) & (buf_size - 1)] = w;
    w = w >> 8;
  }
}

static void
stm32f4_ep_write1(device_t *dev, usb_ep_t *ue,
                  size_t len, uint8_t (*getu8)(void *opaque), void *opaque)
{
  const uint32_t ep = ue->ue_address & 0x7f;
  fifo_write_fn(ep, getu8, opaque, len);
}

static void
stm32f4_ep_cnak(device_t *dev, usb_ep_t *ue)
{
  const uint32_t ep = ue->ue_address & 0x7f;
  ep_cnak(ep);
}



static int
stm32f4_ep_avail_bytes(device_t *dev, usb_ep_t *ue)
{
  const uint32_t ep = ue->ue_address & 0x7f;
  if(reg_rd(OTG_FS_DIEPCTL(ep)) & (1 << 31))
    return 0;
  const int avail_words = reg_rd(OTG_FS_DTXFSTS(ep));
  const int avail_bytes = avail_words * 4;
  return avail_bytes;
}



static const usb_ctrl_vtable_t stm32f4_otgfs_vtable =
{
  .read = stm32f4_ep_read,
  .write = stm32f4_ep_write,
  .write1 = stm32f4_ep_write1,
  .cnak = stm32f4_ep_cnak,
  .avail_bytes = stm32f4_ep_avail_bytes,
};


void
init_interfaces(usb_ctrl_t *uc)
{
  size_t total_desc_size = sizeof(struct usb_config_descriptor);

  int ep_in_index = 1;
  int ep_out_index = 1;

  usb_interface_t *ui;
  int num_interfaces = 0;

  STAILQ_FOREACH(ui, &uc->uc_ifaces, ui_link) {
    total_desc_size += ui->ui_gen_desc(NULL, ui->ui_opaque, 0);
    total_desc_size +=
      ui->ui_num_endpoints * sizeof(struct usb_endpoint_descriptor);
    num_interfaces++;
  }

  uc->uc_config_desc_size = total_desc_size;
  uc->uc_config_desc = calloc(1, total_desc_size);
  void *o = uc->uc_config_desc;

  struct usb_config_descriptor *ucd = o;

  ucd->bLength = sizeof(struct usb_config_descriptor);
  ucd->bDescriptorType = USB_DESC_TYPE_CONFIGURATION;
  ucd->wTotalLength = total_desc_size;
  ucd->bNumInterfaces = num_interfaces;
  ucd->bConfigurationValue = 1;
  ucd->bmAttributes = 0xc0; // Self powered
  ucd->bMaxPower = 100 / 2; // 100mW

  o += sizeof(struct usb_config_descriptor);

  int iface_index = 0;

  STAILQ_FOREACH(ui, &uc->uc_ifaces, ui_link) {

    o += ui->ui_gen_desc(o, ui->ui_opaque, iface_index);
    iface_index++;

    for(size_t j = 0; j < ui->ui_num_endpoints; j++)  {
      usb_ep_t *ue = ui->ui_endpoints + j;
      assert(ue->ue_max_packet_size < 2048);
      ue->ue_dev = &uc->uc_dev;
      ue->ue_vtable = &stm32f4_otgfs_vtable;
      ue->ue_name = ui->ui_name;

      if(ue->ue_address & 0x80) {
        if(ep_in_index == uc->uc_num_endpoints)
          panic("stm32f4_otgfs: Not enough %s endpoints", "IN");
        // IN
        ue->ue_address |= ep_in_index;
        uc->uc_in_ue[ep_in_index] = ue;
        ep_in_index++;

      } else {
        // OUT
        if(ep_out_index == uc->uc_num_endpoints)
          panic("stm32f4_otgfs: Not enough %s endpoints", "OUT");
        ue->ue_address |= ep_out_index;
        uc->uc_out_ue[ep_out_index] = ue;
        ep_out_index++;

      }

      struct usb_endpoint_descriptor *ued = o;
      ued->bLength = sizeof(struct usb_endpoint_descriptor);
      ued->bDescriptorType = USB_DESC_TYPE_ENDPOINT;
      ued->bEndpointAddress = ue->ue_address;
      ued->bmAttributes = ue->ue_endpoint_type;
      ued->wMaxPacketSize = ue->ue_max_packet_size;
      ued->bInterval = ue->ue_interval;

      o += sizeof(struct usb_endpoint_descriptor);
    }
  }

  assert(o == uc->uc_config_desc + uc->uc_config_desc_size);
}


static void
usb_print_info(struct device *d, struct stream *st)
{
  struct usb_ctrl *uc = (struct usb_ctrl *)d;

  const uint32_t dsts = reg_rd(OTG_FS_DSTS);
  stprintf(st, "\tCore status: %s   D-:%d  D+:%d  Suspend:%d\n",
           dsts & 0x8 ? "Error" : "OK",
           ((dsts >> 22) & 1),
           ((dsts >> 23) & 1),
           dsts & 1);

  stprintf(st, "\tResets: %d  Enumerations: %d  Core errors: %d\n",
           uc->uc_resets,
           uc->uc_enumerations,
           uc->uc_erratic_errors);

  if(!(dsts & 1)) {
    stprintf(st, "\tAssigned address: %d   Last SOF Frame: %d\n",
             uc->uc_assigned_addr,
             (dsts >> 8) & 0x3fff);
  } else {
    stprintf(st, "\tDisconnected\n");
  }

  stprintf(st, "\tIN Endpoints: (TX)\n");
  for(int i = 0; i < uc->uc_num_endpoints; i++) {
    uint32_t diepctl = reg_rd(OTG_FS_DIEPCTL(i));

    char status[8];

    status[0] = diepctl & (1 << 31) ? 'E' : ' ';
    status[1] = diepctl & (1 << 30) ? 'D' : ' ';
    status[2] = diepctl & (1 << 27) ? 'S' : ' ';
    status[3] = diepctl & (1 << 26) ? 'C' : ' ';
    status[4] = diepctl & (1 << 21) ? 'T' : ' ';
    status[5] = diepctl & (1 << 17) ? 'N' : ' ';
    status[6] = diepctl & (1 << 15) ? 'A' : ' ';
    status[7] = 0;


    stprintf(st, "\t\t[%d] = %3d 0x%08x 0x%08x %s %d ",
             i,
             reg_rd(OTG_FS_DTXFSTS(i)),
             reg_rd(OTG_FS_DIEPINT(i)),
             diepctl,
             status,
             (diepctl >> 22) & 0xf);

    usb_ep_t *ue = uc->uc_in_ue[i];
    if(ue != NULL) {
      stprintf(st, "%-10u %-10u %s\n",
               ue->ue_num_packets,
               ue->ue_num_drops,
               ue->ue_name);
      ue->ue_num_packets = 0;
      ue->ue_num_drops = 0;
    } else {
      stprintf(st, "\n");
    }
  }
}


static void
probe_endpoints(usb_ctrl_t *uc)
{
  uc->uc_num_endpoints = 1;
  for(int i = 1; i < MAX_NUM_ENDPOINTS; i++) {
    if(!(reg_rd(OTG_FS_DIEPTXF(i)) & 0xffff))
      break;
    uc->uc_num_endpoints++;
  }

  printf("stm32f4_otgfs: %d endpoints\n", uc->uc_num_endpoints);
}


static const device_class_t stm32f4_otgfs_class = {
  .dc_print_info = usb_print_info
};


static void
disconnect(task_t *t)
{
  usb_ctrl_t *uc = &g_usb_ctrl;
  reset_peripheral(RST_OTGFS);
  while(!reg_get_bit(OTG_FS_GRSTCTL, 31)) {}
  stm32f4_otgfs_init_regs(uc);
}


static void
reconnect(void *opaque, uint64_t expire)
{
  reg_set_bits(OTG_FS_DCTL, 1, 1, 0); // No soft disconnect
}


void
stm32f4_otgfs_create(uint16_t vid, uint16_t pid,
                     const char *manfacturer_string,
                     const char *product_string,
                     struct usb_interface_queue *q)
{
  gpio_conf_af(GPIO_PA(11), 10, GPIO_OPEN_DRAIN,
               GPIO_SPEED_VERY_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PA(12), 10, GPIO_OPEN_DRAIN,
               GPIO_SPEED_VERY_HIGH, GPIO_PULL_NONE);

  usb_ctrl_t *uc = &g_usb_ctrl;

  uc->uc_ifaces = *q;
  STAILQ_INIT(q);

  uc->uc_dev.d_class = &stm32f4_otgfs_class;
  uc->uc_dev.d_name = "usb";
  device_register(&uc->uc_dev);

  uc->uc_manufacturer = manfacturer_string;
  uc->uc_product = product_string;
  uc->uc_desc.idVendor = vid;
  uc->uc_desc.idProduct = pid;

  clk_enable(CLK_OTGFS);

  init_interfaces(uc);

  reset_peripheral(RST_OTGFS);
  while(!reg_get_bit(OTG_FS_GRSTCTL, 31)) {}
  probe_endpoints(uc);
  stm32f4_otgfs_init_regs(uc);

  uc->uc_reconnect.t_cb = reconnect;

  uc->uc_softirq.t_run = disconnect;

  irq_enable(67, IRQ_LEVEL_NET);
}


#include <mios/cli.h>

static error_t
usbreset(cli_t *cli, int argc, char **argv)
{
  task_run(&g_usb_ctrl.uc_softirq);
  return 0;
}

CLI_CMD_DEF("usbreset", usbreset);
