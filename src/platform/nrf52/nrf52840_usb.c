// USB device controller for the nRF52840 USBD peripheral, wired to the mios
// USB stack. The stack contract (usb_ctrl_vtable_t + ue_completed callbacks,
// in-driver EP0 state machine) matches the STM32 drivers; what differs is the
// hardware model:
//   - EasyDMA per endpoint (EPIN[n]/EPOUT[n].PTR/MAXCNT) instead of a shared
//     packet SRAM window,
//   - SET_ADDRESS is handled in silicon (no EP0SETUP for it, no deferred
//     address write),
//   - control transfers are driven by tasks/events (EP0SETUP, EP0DATADONE,
//     EP0RCVOUT, EP0STATUS) rather than a control EPnR register,
//   - bring-up needs the USB regulator handshake via the POWER peripheral plus
//     the anomaly 187 workaround around ENABLE, or the device never enumerates.
//
// HW-untested: verified to build/link; enumeration needs on-device iteration.

#include <mios/io.h>
#include <mios/sys.h>
#include "irq.h"

#include <usb/usb.h>
#include <usb/usb_desc.h>

#include <sys/param.h>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "nrf52_reg.h"
#include "nrf52840_usb.h"

#define USBD_BASE 0x40027000

// Tasks
#define USBD_TASKS_STARTEPIN(n)  (USBD_BASE + 0x004 + (n) * 4)
#define USBD_TASKS_STARTEPOUT(n) (USBD_BASE + 0x028 + (n) * 4)
#define USBD_TASKS_EP0RCVOUT     (USBD_BASE + 0x04c)
#define USBD_TASKS_EP0STATUS     (USBD_BASE + 0x050)
#define USBD_TASKS_EP0STALL      (USBD_BASE + 0x054)

// Events
#define USBD_EVENTS_USBRESET     (USBD_BASE + 0x100)
#define USBD_EVENTS_STARTED      (USBD_BASE + 0x104)
#define USBD_EVENTS_ENDEPIN(n)   (USBD_BASE + 0x108 + (n) * 4)
#define USBD_EVENTS_EP0DATADONE  (USBD_BASE + 0x128)
#define USBD_EVENTS_ENDEPOUT(n)  (USBD_BASE + 0x130 + (n) * 4)
#define USBD_EVENTS_SOF          (USBD_BASE + 0x154)
#define USBD_EVENTS_USBEVENT     (USBD_BASE + 0x158)
#define USBD_EVENTS_EP0SETUP     (USBD_BASE + 0x15c)
#define USBD_EVENTS_EPDATA       (USBD_BASE + 0x160)

// Registers
#define USBD_INTEN          (USBD_BASE + 0x300)
#define USBD_INTENSET       (USBD_BASE + 0x304)
#define USBD_INTENCLR       (USBD_BASE + 0x308)
#define USBD_EVENTCAUSE     (USBD_BASE + 0x400)
#define USBD_EPSTATUS       (USBD_BASE + 0x468)
#define USBD_EPDATASTATUS   (USBD_BASE + 0x46c)
#define USBD_USBADDR        (USBD_BASE + 0x470)
#define USBD_BMREQUESTTYPE  (USBD_BASE + 0x480)
#define USBD_BREQUEST       (USBD_BASE + 0x484)
#define USBD_WVALUEL        (USBD_BASE + 0x488)
#define USBD_WVALUEH        (USBD_BASE + 0x48c)
#define USBD_WINDEXL        (USBD_BASE + 0x490)
#define USBD_WINDEXH        (USBD_BASE + 0x494)
#define USBD_WLENGTHL       (USBD_BASE + 0x498)
#define USBD_WLENGTHH       (USBD_BASE + 0x49c)
#define USBD_SIZE_EPOUT(n)  (USBD_BASE + 0x4a0 + (n) * 4)
#define USBD_ENABLE         (USBD_BASE + 0x500)
#define USBD_USBPULLUP      (USBD_BASE + 0x504)
#define USBD_EPINEN         (USBD_BASE + 0x510)
#define USBD_EPOUTEN        (USBD_BASE + 0x514)
#define USBD_EPIN_PTR(n)    (USBD_BASE + 0x600 + (n) * 0x14)
#define USBD_EPIN_MAXCNT(n) (USBD_BASE + 0x604 + (n) * 0x14)
#define USBD_EPOUT_PTR(n)   (USBD_BASE + 0x700 + (n) * 0x14)
#define USBD_EPOUT_MAXCNT(n)(USBD_BASE + 0x704 + (n) * 0x14)

// EVENTCAUSE bits
#define EVENTCAUSE_READY  (1 << 11)

// POWER peripheral: USB supply regulator
#define POWER_BASE               0x40000000
#define POWER_EVENTS_USBDETECTED (POWER_BASE + 0x11c)
#define POWER_EVENTS_USBPWRRDY   (POWER_BASE + 0x124)
#define POWER_USBREGSTATUS       (POWER_BASE + 0x438)
#define USBREGSTATUS_VBUSDETECT  (1 << 0)
#define USBREGSTATUS_OUTPUTRDY   (1 << 1)

#define USBD_IRQ 39

// EP0 control endpoint uses 64-byte packets on the nRF52840.
#define EP0_MPS 64

#define MAX_NUM_ENDPOINTS 8


typedef struct usb_tx_ptr {
  const void *ptr;
  uint8_t (*getch)(void *opaque);
  uint16_t offset;
  uint16_t total;
  uint8_t zlp;
} usb_tx_ptr_t;


typedef struct ep0_in {
  usb_ep_t ue;
  usb_tx_ptr_t tp;
} ep0_in_t;

typedef struct ep0_out {
  usb_ep_t ue;
} ep0_out_t;


typedef struct usb_dev {
  device_t ud_dev;

  struct usb_device_descriptor ud_desc;
  const char *ud_manufacturer;
  const char *ud_product;
  uint8_t *ud_config_desc;
  size_t ud_config_desc_size;
} usb_dev_t;


typedef struct usb_ctrl {

  usb_dev_t uc_ud;

  usb_ep_t *uc_ue[2][MAX_NUM_ENDPOINTS];  // [0]=IN, [1]=OUT

  ep0_in_t uc_ep0_in;
  ep0_out_t uc_ep0_out;

  uint8_t uc_ep0_out_pending; // EP0DATADONE will end an OUT data stage

  uint8_t uc_txbusy[MAX_NUM_ENDPOINTS]; // IN endpoint DMA in flight

  // EasyDMA buffers (must live in RAM). One max-packet buffer per endpoint.
  uint8_t uc_rxbuf[MAX_NUM_ENDPOINTS][64];
  uint8_t uc_txbuf[MAX_NUM_ENDPOINTS][64];

  struct usb_interface_queue uc_ifaces;

} usb_ctrl_t;


// --- EP0 control transfers -------------------------------------------------

static void
ep0_status(void)
{
  reg_wr(USBD_TASKS_EP0STATUS, 1);
}

static void
ep0_tx_chunk(usb_ctrl_t *uc)
{
  usb_tx_ptr_t *tp = &uc->uc_ep0_in.tp;
  uint8_t *dst = uc->uc_txbuf[0];

  int len = MIN(EP0_MPS, tp->total - tp->offset);
  for(int i = 0; i < len; i++)
    dst[i] = tp->getch(tp);

  reg_wr(USBD_EPIN_PTR(0), (uint32_t)(intptr_t)dst);
  reg_wr(USBD_EPIN_MAXCNT(0), len);
  reg_wr(USBD_TASKS_STARTEPIN(0), 1);
}

static uint8_t
read_tx_ptr(void *opaque)
{
  usb_tx_ptr_t *tp = opaque;
  const uint8_t *data = tp->ptr;
  return data[tp->offset++];
}

static uint8_t
read_string_as_desc(void *opaque)
{
  usb_tx_ptr_t *tp = opaque;
  const uint8_t *data = tp->ptr;
  uint8_t r;

  switch(tp->offset) {
  case 0: r = tp->total; break;
  case 1: r = USB_DESC_TYPE_STRING; break;
  default:
    r = (tp->offset & 1) ? 0 : data[(tp->offset - 2) / 2];
    break;
  }
  tp->offset++;
  return r;
}

static uint8_t
read_bin2hex_as_desc(void *opaque)
{
  usb_tx_ptr_t *tp = opaque;
  const uint8_t *data = tp->ptr;
  uint8_t r;

  switch(tp->offset) {
  case 0: r = tp->total; break;
  case 1: r = USB_DESC_TYPE_STRING; break;
  default:
    if(tp->offset & 1) {
      r = 0;
    } else {
      const int chof = (tp->offset - 2) / 2;
      const uint8_t b = data[chof / 2] >> (chof & 1 ? 0 : 4);
      r = "0123456789abcdef"[b & 0xf];
    }
    break;
  }
  tp->offset++;
  return r;
}


static void
ep0_start_tx(usb_ctrl_t *uc, const void *desc, size_t desclen,
             uint8_t (*getch)(void *), int reqlen)
{
  usb_tx_ptr_t *tp = &uc->uc_ep0_in.tp;

  tp->offset = 0;
  if(desc == NULL) {
    ep0_status();
    tp->ptr = NULL;
    return;
  }

  tp->ptr = desc;
  tp->getch = getch;
  tp->total = MIN(reqlen, (int)desclen);
  tp->zlp = 0;

  // Short descriptor that is a multiple of the packet size needs a trailing
  // ZLP so the host knows the transfer ended.
  if((int)desclen < reqlen && !(desclen % EP0_MPS))
    tp->zlp = 1;

  ep0_tx_chunk(uc);
}


static void
handle_get_descriptor(usb_ctrl_t *uc, const struct usb_setup_packet *usp)
{
  usb_dev_t *ud = &uc->uc_ud;
  const uint8_t type = usp->value >> 8;
  const uint8_t index = usp->value;

  const void *desc = NULL;
  size_t desclen = 0;
  uint8_t (*getch)(void *opaque) = read_tx_ptr;

  switch(type) {
  case USB_DESC_TYPE_DEVICE:
    if(index == 0) {
      desc = &ud->ud_desc;
      desclen = sizeof(struct usb_device_descriptor);
    }
    break;

  case USB_DESC_TYPE_CONFIGURATION:
    if(index == 0) {
      desc = ud->ud_config_desc;
      desclen = ud->ud_config_desc_size;
    }
    break;

  case USB_DESC_TYPE_STRING:
    getch = read_string_as_desc;
    switch(index) {
    case 0:
      desc = "\x09\x04";
      desclen = 2;
      break;
    case 1:
      desc = ud->ud_manufacturer;
      desclen = strlen(desc);
      break;
    case 2:
      desc = ud->ud_product;
      desclen = strlen(desc);
      break;
    case 3:
      getch = read_bin2hex_as_desc;
      const struct serial_number sn = sys_get_serial_number();
      desc = sn.data;
      desclen = sn.len * 2;
      break;
    }
    desclen = desclen * 2 + 2;
    break;

  default:
    break;
  }

  ep0_start_tx(uc, desc, desclen, getch, usp->length);
}


static void enable_endpoints(usb_ctrl_t *uc);

static void
ep0_handle_setup(usb_ctrl_t *uc, const struct usb_setup_packet *usp)
{
  const int recipient = usp->request_type & 0x1f;
  const int type = (usp->request_type >> 5) & 3;
  const int dir_in = usp->request_type & 0x80;

  if(type == 0 && recipient == 0) {
    switch(usp->request) {
    case USB_REQ_GET_STATUS: {
      static const uint8_t zero[2] = {0, 0};
      ep0_start_tx(uc, zero, 2, read_tx_ptr, usp->length);
      break;
    }
    case USB_REQ_GET_DESCRIPTOR:
      handle_get_descriptor(uc, usp);
      break;
    case USB_REQ_SET_CONFIG:
      enable_endpoints(uc);
      ep0_status();
      break;
    default:
      // SET_ADDRESS is handled by hardware and never lands here.
      ep0_status();
      break;
    }
    return;
  }

  if(type == 1 && recipient == 1) {
    usb_interface_t *ui;
    STAILQ_FOREACH(ui, &uc->uc_ifaces, ui_link) {
      if(ui->ui_index == usp->index && ui->ui_iface_cfg)
        ui->ui_iface_cfg(ui->ui_opaque, usp->request, usp->value);
    }
  }

  if(!dir_in && usp->length) {
    // Host->device data stage (e.g. CDC SET_LINE_CODING). Accept and discard
    // the data; the request's meaning was taken from wValue above.
    uc->uc_ep0_out_pending = 1;
    reg_wr(USBD_TASKS_EP0RCVOUT, 1);
  } else {
    ep0_status();
  }
}


static void
ep0_setup(usb_ctrl_t *uc)
{
  struct usb_setup_packet usp;
  usp.request_type = reg_rd(USBD_BMREQUESTTYPE);
  usp.request      = reg_rd(USBD_BREQUEST);
  usp.value        = reg_rd(USBD_WVALUEL) | (reg_rd(USBD_WVALUEH) << 8);
  usp.index        = reg_rd(USBD_WINDEXL) | (reg_rd(USBD_WINDEXH) << 8);
  usp.length       = reg_rd(USBD_WLENGTHL) | (reg_rd(USBD_WLENGTHH) << 8);
  ep0_handle_setup(uc, &usp);
}


static void
ep0_datadone(usb_ctrl_t *uc)
{
  if(uc->uc_ep0_out_pending) {
    // Host->device data packet is in the peripheral buffer. Move it to RAM
    // via EasyDMA; ENDEPOUT[0] then drives the status stage. (The data itself
    // is discarded; its meaning was taken from wValue at setup time.)
    reg_wr(USBD_EPOUT_PTR(0), (uint32_t)(intptr_t)uc->uc_rxbuf[0]);
    reg_wr(USBD_EPOUT_MAXCNT(0), 64);
    reg_wr(USBD_TASKS_STARTEPOUT(0), 1);
    return;
  }

  usb_tx_ptr_t *tp = &uc->uc_ep0_in.tp;
  if(tp->ptr == NULL)
    return;

  if(tp->offset != tp->total) {
    ep0_tx_chunk(uc);
  } else if(tp->zlp) {
    tp->zlp = 0;
    reg_wr(USBD_EPIN_MAXCNT(0), 0);
    reg_wr(USBD_TASKS_STARTEPIN(0), 1);
  } else {
    tp->ptr = NULL;
    ep0_status();
  }
}


// --- Data endpoints --------------------------------------------------------

static void
enable_endpoints(usb_ctrl_t *uc)
{
  uint32_t epinen = 1;  // EP0 IN always on
  uint32_t epouten = 1; // EP0 OUT always on

  for(int ea = 1; ea < MAX_NUM_ENDPOINTS; ea++) {
    usb_ep_t *in  = uc->uc_ue[0][ea];
    usb_ep_t *out = uc->uc_ue[1][ea];

    if(in) {
      epinen |= 1 << ea;
      uc->uc_txbusy[ea] = 0;
      in->ue_running = 1;
      if(in->ue_reset)
        in->ue_reset(&uc->uc_ud.ud_dev, in);
    }
    if(out) {
      epouten |= 1 << ea;
      // Arm reception: point EasyDMA at our buffer for the first OUT packet.
      reg_wr(USBD_EPOUT_PTR(ea), (uint32_t)(intptr_t)uc->uc_rxbuf[ea]);
      reg_wr(USBD_EPOUT_MAXCNT(ea), out->ue_max_packet_size);
      out->ue_running = 1;
      if(out->ue_reset)
        out->ue_reset(&uc->uc_ud.ud_dev, out);
    }
  }

  reg_wr(USBD_EPINEN, epinen);
  reg_wr(USBD_EPOUTEN, epouten);

  // An OUT endpoint NAKs every OUT token until armed by writing SIZE.EPOUT.
  // Arm each one so the first host->device packet is accepted; it is re-armed
  // after each packet is consumed (see nrf_ep_cnak).
  for(int ea = 1; ea < MAX_NUM_ENDPOINTS; ea++) {
    if(uc->uc_ue[1][ea])
      reg_wr(USBD_SIZE_EPOUT(ea), 0);
  }
}


static void
reset_endpoints(usb_ctrl_t *uc)
{
  for(int ea = 1; ea < MAX_NUM_ENDPOINTS; ea++) {
    usb_ep_t *in  = uc->uc_ue[0][ea];
    usb_ep_t *out = uc->uc_ue[1][ea];
    if(in) {
      in->ue_running = 0;
      uc->uc_txbusy[ea] = 0;
      if(in->ue_reset)
        in->ue_reset(&uc->uc_ud.ud_dev, in);
    }
    if(out && out->ue_reset)
      out->ue_reset(&uc->uc_ud.ud_dev, out);
  }
}


// Host acked our IN data (bit n) or sent OUT data (bit 16+n).
static void
handle_epdata(usb_ctrl_t *uc)
{
  const uint32_t status = reg_rd(USBD_EPDATASTATUS);
  reg_wr(USBD_EPDATASTATUS, status); // write 1 to clear

  for(int ea = 1; ea < MAX_NUM_ENDPOINTS; ea++) {
    if(status & (1 << ea)) {
      // IN transfer completed / acked -> TX buffer free.
      uc->uc_txbusy[ea] = 0;
      usb_ep_t *ep = uc->uc_ue[0][ea];
      if(ep && ep->ue_completed)
        ep->ue_completed(&uc->uc_ud.ud_dev, ep, 0, 0);
    }
    if(status & (1 << (16 + ea))) {
      // OUT data waiting in the peripheral buffer: DMA it into RAM.
      usb_ep_t *ep = uc->uc_ue[1][ea];
      if(ep) {
        reg_wr(USBD_EPOUT_PTR(ea), (uint32_t)(intptr_t)uc->uc_rxbuf[ea]);
        reg_wr(USBD_EPOUT_MAXCNT(ea), ep->ue_max_packet_size);
        reg_wr(USBD_TASKS_STARTEPOUT(ea), 1);
      }
    }
  }
}


static void
handle_endepout(usb_ctrl_t *uc, int ea)
{
  usb_ep_t *ep = uc->uc_ue[1][ea];
  if(ep == NULL)
    return;
  const int len = reg_rd(USBD_SIZE_EPOUT(ea));
  if(ep->ue_completed)
    ep->ue_completed(&uc->uc_ud.ud_dev, ep, len, 0);
}


// --- vtable ----------------------------------------------------------------

static void
nrf_ep_read(device_t *dev, usb_ep_t *ue,
            uint8_t *buf, size_t buf_size, size_t buf_offset, size_t bytes)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)dev;
  const uint32_t ea = ue->ue_address & 0x7f;
  const uint8_t *src = uc->uc_rxbuf[ea];
  for(size_t i = 0; i < bytes; i++)
    buf[(buf_offset + i) & (buf_size - 1)] = src[i];
}

static int
nrf_ep_avail_bytes(device_t *dev, usb_ep_t *ue)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)dev;
  const uint32_t ea = ue->ue_address & 0x7f;
  return uc->uc_txbusy[ea] ? 0 : ue->ue_max_packet_size;
}

static void
nrf_ep_start_tx(usb_ctrl_t *uc, int ea, size_t len)
{
  uc->uc_txbusy[ea] = 1;
  reg_wr(USBD_EPIN_PTR(ea), (uint32_t)(intptr_t)uc->uc_txbuf[ea]);
  reg_wr(USBD_EPIN_MAXCNT(ea), len);
  reg_wr(USBD_TASKS_STARTEPIN(ea), 1);
}

static error_t
nrf_ep_write(device_t *dev, usb_ep_t *ue, const uint8_t *buf, size_t len)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)dev;
  const uint32_t ea = ue->ue_address & 0x7f;
  if(uc->uc_txbusy[ea])
    return ERR_NOT_READY;
  if(len > ue->ue_max_packet_size)
    len = ue->ue_max_packet_size;
  memcpy(uc->uc_txbuf[ea], buf, len);
  nrf_ep_start_tx(uc, ea, len);
  return 0;
}

static void
nrf_ep_write1(device_t *dev, usb_ep_t *ue,
              size_t len, uint8_t (*getu8)(void *opaque), void *opaque)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)dev;
  const uint32_t ea = ue->ue_address & 0x7f;
  if(len > ue->ue_max_packet_size)
    len = ue->ue_max_packet_size;
  for(size_t i = 0; i < len; i++)
    uc->uc_txbuf[ea][i] = getu8(opaque);
  nrf_ep_start_tx(uc, ea, len);
}

static void
nrf_ep_cnak(device_t *dev, usb_ep_t *ue)
{
  // Re-arm the OUT endpoint to accept the next host->device packet by writing
  // SIZE.EPOUT (see enable_endpoints).
  const uint32_t ea = ue->ue_address & 0x7f;
  reg_wr(USBD_SIZE_EPOUT(ea), 0);
}


static const usb_ctrl_vtable_t nrf_usb_vtable = {
  .read = nrf_ep_read,
  .write = nrf_ep_write,
  .write1 = nrf_ep_write1,
  .cnak = nrf_ep_cnak,
  .avail_bytes = nrf_ep_avail_bytes,
};


static usb_ctrl_t g_usb_ctrl = {
  .uc_ud = {
    .ud_desc = {
      .bLength            = sizeof(struct usb_device_descriptor),
      .bDescriptorType    = USB_DESC_TYPE_DEVICE,
      .bcdUSB             = 0x200,
      .bDeviceClass       = 0xef,   // misc / IAD
      .bDeviceSubClass    = 0x02,
      .bDeviceProtocol    = 0x01,
      .bMaxPacketSize0    = EP0_MPS,
      .bcdDevice          = 0x100,
      .iManufacturer      = 1,
      .iProduct           = 2,
      .iSerialNumber      = 3,
      .bNumConfigurations = 1,
    },
  },
  .uc_ue[0][0] = &g_usb_ctrl.uc_ep0_in.ue,
  .uc_ue[1][0] = &g_usb_ctrl.uc_ep0_out.ue,
  .uc_ep0_in  = { .ue = { .ue_name = "ep0" } },
  .uc_ep0_out = { .ue = { .ue_name = "ep0" } },
};


// USBD is a single peripheral with one vector; dispatch each pending event.
void
irq_39(void)
{
  usb_ctrl_t *uc = &g_usb_ctrl;

  if(reg_rd(USBD_EVENTS_USBRESET)) {
    reg_wr(USBD_EVENTS_USBRESET, 0);
    reset_endpoints(uc);
  }

  if(reg_rd(USBD_EVENTS_EP0SETUP)) {
    reg_wr(USBD_EVENTS_EP0SETUP, 0);
    ep0_setup(uc);
  }

  if(reg_rd(USBD_EVENTS_EP0DATADONE)) {
    reg_wr(USBD_EVENTS_EP0DATADONE, 0);
    ep0_datadone(uc);
  }

  if(reg_rd(USBD_EVENTS_ENDEPOUT(0))) {
    reg_wr(USBD_EVENTS_ENDEPOUT(0), 0);
    // EP0 OUT data DMA'd to RAM: finish the control-write status stage.
    if(uc->uc_ep0_out_pending) {
      uc->uc_ep0_out_pending = 0;
      ep0_status();
    }
  }

  for(int n = 1; n < MAX_NUM_ENDPOINTS; n++) {
    if(reg_rd(USBD_EVENTS_ENDEPOUT(n))) {
      reg_wr(USBD_EVENTS_ENDEPOUT(n), 0);
      handle_endepout(uc, n);
    }
  }

  if(reg_rd(USBD_EVENTS_EPDATA)) {
    reg_wr(USBD_EVENTS_EPDATA, 0);
    handle_epdata(uc);
  }

  if(reg_rd(USBD_EVENTS_USBEVENT)) {
    reg_wr(USBD_EVENTS_USBEVENT, 0);
    reg_rd(USBD_EVENTCAUSE); // suspend/resume/etc: acknowledged, no action
  }

  // IN DMA-complete events need no handling; the host ack comes via EPDATA.
  for(int n = 0; n < MAX_NUM_ENDPOINTS; n++) {
    if(reg_rd(USBD_EVENTS_ENDEPIN(n)))
      reg_wr(USBD_EVENTS_ENDEPIN(n), 0);
  }
}


static int
alloc_ep(usb_ctrl_t *uc, int out, usb_ep_t *ep)
{
  for(int i = 1; i < MAX_NUM_ENDPOINTS; i++) {
    if(uc->uc_ue[out][i] == NULL) {
      uc->uc_ue[out][i] = ep;
      return i;
    }
  }
  panic("usb: out of endpoints");
}


static void
init_interfaces(usb_ctrl_t *uc)
{
  size_t total = sizeof(struct usb_config_descriptor);
  int num_interfaces = 0;

  usb_interface_t *ui;
  STAILQ_FOREACH(ui, &uc->uc_ifaces, ui_link) {
    total += ui->ui_gen_desc(NULL, ui->ui_opaque, 0);
    total += ui->ui_num_endpoints * sizeof(struct usb_endpoint_descriptor);
    num_interfaces++;
  }

  uc->uc_ud.ud_config_desc_size = total;
  uc->uc_ud.ud_config_desc = calloc(1, total);
  void *o = uc->uc_ud.ud_config_desc;

  struct usb_config_descriptor *ucd = o;
  ucd->bLength = sizeof(struct usb_config_descriptor);
  ucd->bDescriptorType = USB_DESC_TYPE_CONFIGURATION;
  ucd->wTotalLength = total;
  ucd->bNumInterfaces = num_interfaces;
  ucd->bConfigurationValue = 1;
  ucd->bmAttributes = 0xc0; // self powered
  ucd->bMaxPower = 100 / 2;
  o += sizeof(struct usb_config_descriptor);

  int iface_index = 0;
  STAILQ_FOREACH(ui, &uc->uc_ifaces, ui_link) {
    o += ui->ui_gen_desc(o, ui->ui_opaque, iface_index);
    ui->ui_index = iface_index++;

    for(size_t j = 0; j < ui->ui_num_endpoints; j++) {
      usb_ep_t *ue = ui->ui_endpoints + j;
      assert(ue->ue_max_packet_size <= 64);
      ue->ue_dev = &uc->uc_ud.ud_dev;
      ue->ue_vtable = &nrf_usb_vtable;
      ue->ue_name = ui->ui_name;

      const int out = !(ue->ue_address & 0x80);
      const int ea = alloc_ep(uc, out, ue);
      ue->ue_address = (ue->ue_address & 0x80) | ea;

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

  assert(o == uc->uc_ud.ud_config_desc + uc->uc_ud.ud_config_desc_size);
}


static void
usb_print_info(struct device *d, struct stream *st)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)d;
  stprintf(st, "USBD addr:%d  EPSTATUS:%08x\n",
           reg_rd(USBD_USBADDR), reg_rd(USBD_EPSTATUS));
  for(int i = 0; i < MAX_NUM_ENDPOINTS; i++) {
    usb_ep_t *in = uc->uc_ue[0][i];
    usb_ep_t *out = uc->uc_ue[1][i];
    if(in)
      stprintf(st, "  IN  %d %-10s busy:%d\n", i, in->ue_name, uc->uc_txbusy[i]);
    if(out)
      stprintf(st, "  OUT %d %-10s\n", i, out->ue_name);
  }
}


static const device_class_t nrf52840_usb_class = {
  .dc_class_name = "usb",
  .dc_print_info = usb_print_info,
};


// Anomaly 187: USBD must be enabled with an undocumented tweak or it fails to
// respond. Applied in two halves, before and after the ENABLE handshake.
static void
errata_187_before(void)
{
  *(volatile uint32_t *)0x4006ec00 = 0x00009375;
  *(volatile uint32_t *)0x4006ed14 = 0x00000003;
  *(volatile uint32_t *)0x4006ec00 = 0x00009375;
}

static void
errata_187_after(void)
{
  *(volatile uint32_t *)0x4006ec00 = 0x00009375;
  *(volatile uint32_t *)0x4006ed14 = 0x00000000;
  *(volatile uint32_t *)0x4006ec00 = 0x00009375;
}


void
nrf52840_usb_create(uint16_t vid, uint16_t pid,
                    const char *manufacturer, const char *product,
                    struct usb_interface_queue *q)
{
  usb_ctrl_t *uc = &g_usb_ctrl;
  uc->uc_ifaces = *q;
  STAILQ_INIT(q);

  uc->uc_ep0_in.ue.ue_iface_aux = &uc->uc_ep0_in;
  uc->uc_ep0_out.ue.ue_iface_aux = &uc->uc_ep0_out;

  uc->uc_ud.ud_dev.d_class = &nrf52840_usb_class;
  uc->uc_ud.ud_dev.d_name = "usb";
  device_register(&uc->uc_ud.ud_dev);

  uc->uc_ud.ud_manufacturer = manufacturer;
  uc->uc_ud.ud_product = product;
  uc->uc_ud.ud_desc.idVendor = vid;
  uc->uc_ud.ud_desc.idProduct = pid;

  init_interfaces(uc);

  // Wait for VBUS (the dongle is bus-powered, so this is already asserted).
  while(!(reg_rd(POWER_USBREGSTATUS) & USBREGSTATUS_VBUSDETECT)) {}

  errata_187_before();

  reg_wr(USBD_ENABLE, 1);
  while(!reg_rd(USBD_EVENTS_USBEVENT)) {}
  while(!(reg_rd(USBD_EVENTCAUSE) & EVENTCAUSE_READY)) {}
  reg_wr(USBD_EVENTCAUSE, EVENTCAUSE_READY);
  reg_wr(USBD_EVENTS_USBEVENT, 0);

  errata_187_after();

  // Wait for the USB supply regulator output to settle.
  while(!(reg_rd(POWER_USBREGSTATUS) & USBREGSTATUS_OUTPUTRDY)) {}

  reg_wr(USBD_EPINEN, 1);   // EP0 IN
  reg_wr(USBD_EPOUTEN, 1);  // EP0 OUT

  // INTEN bit = (event_offset - 0x100) / 4.
  reg_wr(USBD_INTENSET,
         (1 << 0)     | // USBRESET
         (1 << 10)    | // EP0DATADONE
         (0xff << 12) | // ENDEPOUT[0..7]
         (1 << 22)    | // USBEVENT
         (1 << 23)    | // EP0SETUP
         (1 << 24));    // EPDATA

  irq_enable(USBD_IRQ, IRQ_LEVEL_NET);

  // Attach D+ pull-up: signal our presence to the host.
  reg_wr(USBD_USBPULLUP, 1);
}
