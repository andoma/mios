#include "stm32g4_usb.h"

#include "stm32g4_clk.h"
#include "stm32g4_reg.h"


#include <mios/io.h>
#include <mios/sys.h>
#include "irq.h"

#include <usb/usb.h>
#include <usb/usb_desc.h>

#include <sys/param.h>

#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define USB_BASE 0x40005c00
#define USB_SRAM 0x40006000


#define USB_EPnR(x) (USB_BASE + (x) * 4)
#define USB_CNTR    (USB_BASE + 0x40)
#define USB_ISTR    (USB_BASE + 0x44)
#define USB_FNR     (USB_BASE + 0x48)
#define USB_DADDR   (USB_BASE + 0x4c)
#define USB_BTABLE  (USB_BASE + 0x50)
#define USB_LPMCSR  (USB_BASE + 0x54)
#define USB_BCDR    (USB_BASE + 0x58)


#define USB_ADDR_TX(x)  (USB_SRAM + (x) * 8 + 0)
#define USB_COUNT_TX(x) (USB_SRAM + (x) * 8 + 2)
#define USB_ADDR_RX(x)  (USB_SRAM + (x) * 8 + 4)
#define USB_COUNT_RX(x) (USB_SRAM + (x) * 8 + 6)

#define MAX_NUM_ENDPOINTS 8

#define EP_TYPE_BULK      0
#define EP_TYPE_CONTROL   1
#define EP_TYPE_ISO       2
#define EP_TYPE_INTERRUPT 3




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

  usb_ep_t *uc_ue[2][MAX_NUM_ENDPOINTS];  // for [2]  0=IN, 1=OUT

  uint16_t uc_epreg[MAX_NUM_ENDPOINTS];

  uint8_t uc_pending_daddr;

  ep0_in_t uc_ep0_in;   // TX
  ep0_out_t uc_ep0_out; // RX

  uint8_t *rxbuf[MAX_NUM_ENDPOINTS];
  uint8_t *txbuf[MAX_NUM_ENDPOINTS];
  uint8_t txsiz[MAX_NUM_ENDPOINTS];

  struct usb_interface_queue uc_ifaces;

} usb_ctrl_t;


static const char *
typestr(uint16_t epnr)
{
  uint16_t type = (epnr >> 9) & 3;
  uint16_t kind = epnr & 0x100;

  switch(type) {
  case 0:
    return kind ? "DBL_BUF" : "BULK";
  case 1:
    return kind ? "CTRL-ONLY" : "CTRL";
  case 2:
    return "ISO";
  case 3:
    return "INTERRUPT";
  }
  return "???";
}

static const char *statbits(int n)
{
  switch(n) {
  case 0: return "Off";
  case 1: return "Stall";
  case 2: return "Nak";
  case 3: return "Valid";
  default: return "???";
  }
}


static void
write_epnr(int ea, uint16_t value)
{
  reg_wr16(USB_EPnR(ea), value);
}


static void
handle_ctr(usb_ctrl_t *uc, uint16_t istr)
{
  const int epa = istr & 0xf;
  if(epa >= MAX_NUM_ENDPOINTS)
    return;

  uint16_t epnr = reg_rd16(USB_EPnR(epa));

  if(epnr & 0x8000) {
    write_epnr(epa, uc->uc_epreg[epa] & ~(1 << 15)); // Clear CTR_RX
    // RX (OUT from host point-of-view)
    usb_ep_t *ep = uc->uc_ue[1][epa];
    if(ep != NULL) {
      const int len = reg_rd16(USB_COUNT_RX(epa)) & 0x3ff;
      const int is_setup = (epnr >> 11) & 1;
      ep->ue_completed(&uc->uc_ud.ud_dev, ep, len, is_setup);
    }
  }

  if(epnr & 0x80) {
    // TX completed (IN from host point-of-view)

    write_epnr(epa, uc->uc_epreg[epa] & ~(1 << 7)); // Clear CTR_TX

    usb_ep_t *ep = uc->uc_ue[0][epa];
    if(ep != NULL && ep->ue_completed) {
      ep->ue_completed(&uc->uc_ud.ud_dev, ep, 0, 0);
    }
  }
}


static void
tx_start(usb_ctrl_t *uc, int ea, usb_tx_ptr_t *tx, int max_packet_size)
{
  uint16_t *dst16 = (uint16_t *)uc->txbuf[ea];

  assert(tx != NULL);
  assert(tx->getch != NULL);
  assert(tx->ptr != NULL);

  int len = MIN(max_packet_size, tx->total - tx->offset);
  reg_wr16(USB_COUNT_TX(ea), len);
  while(len > 1) {
    uint8_t a = tx->getch(tx);
    uint8_t b = tx->getch(tx);
    uint16_t u16 = (b << 8) | a;
    *dst16++ = u16;
    len -= 2;
  }

  if(len) {
    uint8_t a = tx->getch(tx);
    *dst16++ = a;
  }

  write_epnr(ea, uc->uc_epreg[ea] | (1 << 4)); // TX: NAK -> VALID
}

static void
send_zlp(usb_ctrl_t *uc, int ea)
{
  reg_wr16(USB_COUNT_TX(ea), 0);
  write_epnr(ea, uc->uc_epreg[ea] | (1 << 4)); // TX: NAK -> VALID
}


static void
ep0_txco(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)d;
  usb_tx_ptr_t *tp = &uc->uc_ep0_in.tp;


  if(uc->uc_pending_daddr) {
    reg_wr(USB_DADDR, uc->uc_pending_daddr);
    uc->uc_pending_daddr = 0;
  }

  if(tp->ptr == NULL)
    return;

  if(tp->offset != tp->total) {
    tx_start(uc, 0, tp, 8);
  } else if(tp->zlp) {
    tp->zlp = 0;
    send_zlp(uc, 0);
  } else {
    tp->ptr = NULL;
  }
}

uint8_t
read_tx_ptr(void *opaque)
{
  usb_tx_ptr_t *tp = opaque;
  const uint8_t *data = tp->ptr;
  return data[tp->offset++];
}


uint8_t
read_string_as_desc(void *opaque)
{
  usb_tx_ptr_t *tp = opaque;
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
  usb_tx_ptr_t *tp = opaque;
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
handle_get_descriptor(usb_dev_t *ud, const struct usb_setup_packet *usp,
                      usb_tx_ptr_t *tp)
{
  const uint8_t type = usp->value >> 8;
  const uint8_t index = usp->value;

  const void *desc = NULL;
  size_t desclen = 0;

  uint8_t (*getch)(void *opaque) = read_tx_ptr;

  switch(type) {

  case USB_DESC_TYPE_DEVICE:
    if(index != 0)
      break;
    desc = &ud->ud_desc;
    desclen = sizeof(struct usb_device_descriptor);
    break;

  case USB_DESC_TYPE_CONFIGURATION:
    if(index != 0)
      break;
    desc = ud->ud_config_desc;
    desclen = ud->ud_config_desc_size;
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

  tp->offset = 0;

  if(desc == NULL) {
    send_zlp((usb_ctrl_t *)ud, 0);
    return;
  }

  const int reqlen = usp->length;

  tp->ptr = desc;
  tp->getch = getch;
  tp->total = MIN(reqlen, desclen);
  tp->zlp = 0;

  // If the descriptor is shorter then asked for and is a multiple of 8
  // we must add a trailing ZLP
  if(desclen < reqlen && !(desclen & 7))
    tp->zlp = 1;

  tx_start((usb_ctrl_t *)ud, 0, tp, 8);
}


static void
enable_endpoints(usb_ctrl_t *uc)
{
  for(int ea = 1; ea < MAX_NUM_ENDPOINTS; ea++) {
    int bits = 0;
    usb_ep_t *in  = uc->uc_ue[0][ea];
    usb_ep_t *out = uc->uc_ue[1][ea];
    if(out) {
      bits |= (3 << 12);  // RX: OFF -> VALID
    }

    if(in) {
      bits |= (2 << 4);  // TX: OFF -> NAK
    }

    write_epnr(ea, uc->uc_epreg[ea] | bits);

    if(in) {
      in->ue_running = 1;
      if(in->ue_reset)
        in->ue_reset(&uc->uc_ud.ud_dev, in);
    }
    if(out) {
      out->ue_running = 1;
      if(out->ue_reset)
        out->ue_reset(&uc->uc_ud.ud_dev, out);
    }
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
      if(in->ue_reset)
        in->ue_reset(&uc->uc_ud.ud_dev, in);
    }
    if(out) {
      out->ue_running = 0;
      if(out->ue_reset)
        out->ue_reset(&uc->uc_ud.ud_dev, out);
    }
  }
}


static void
ep0_handle_setup(usb_ctrl_t *uc, const struct usb_setup_packet *usp)
{
  usb_tx_ptr_t *tp = &uc->uc_ep0_in.tp;
  const int recipient = usp->request_type & 0x1f;
  const int type = (usp->request_type >> 5) & 3;

  if(type == 0 && recipient == 0) {

    switch(usp->request) {
    case USB_REQ_GET_STATUS:
      send_zlp(uc, 0);
      break;

    case USB_REQ_SET_ADDRESS:
      send_zlp(uc, 0);
      uc->uc_pending_daddr = 0x80 | usp->value;
      break;

    case USB_REQ_GET_DESCRIPTOR:
      handle_get_descriptor(&uc->uc_ud, usp, tp);
      break;

    case USB_REQ_SET_CONFIG:
      send_zlp(uc, 0);
      enable_endpoints(uc);
      break;

    default:
      panic("Can't handle setup %d",
            usp->request);
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
  send_zlp(uc, 0);
}



static void
ep0_rx(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)d;
  struct usb_setup_packet *usp = (struct usb_setup_packet *)uc->rxbuf[0];

  if(bytes == 8) {
    ep0_handle_setup(uc, usp);
  }
  write_epnr(0, uc->uc_epreg[0] | (1 << 12)); // RX: NAK -> VALID
}



static usb_ctrl_t g_usb_ctrl = {
  .uc_ud = {
    .ud_desc = {
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
  },

  .uc_ue[0][0] = &g_usb_ctrl.uc_ep0_in.ue,
  .uc_ue[1][0] = &g_usb_ctrl.uc_ep0_out.ue,
  .uc_epreg[0] = EP_TYPE_CONTROL << 9 | (1 << 15) | (1 << 7) | (1 << 8),

  .uc_ep0_in = {
    .ue = {
      .ue_iface_aux = &g_usb_ctrl.uc_ep0_in,
      .ue_completed = ep0_txco,
      .ue_name = "ep0",
    }
  },

  .uc_ep0_out = {
    .ue = {
      .ue_iface_aux = &g_usb_ctrl.uc_ep0_out,
      .ue_completed = ep0_rx,
      .ue_name = "ep0",
    }
  }
};

void
irq_19(void) // High prio interrupts
{
  uint16_t irq = reg_rd(USB_ISTR);
  panic("i19:%x", irq);
  handle_ctr(&g_usb_ctrl, irq);
}

void
irq_20(void) // Low prio interrupts
{
  uint16_t irq = reg_rd(USB_ISTR);
  if(irq & 0x400) {
    usb_ctrl_t *uc = &g_usb_ctrl;
    uc->uc_pending_daddr = 0;
    reset_endpoints(uc);
    reg_wr(USB_ISTR, ~0x400);
    // Reset EP0
    reg_wr16(USB_EPnR(0),
             (3 << 12) |  // RX: VALID
             (1 << 9) |   // CONTROL
             (2 << 4) |  // TX: NAK
             0);
    reg_wr(USB_DADDR, 0x80); // Set EF, addr=0
  } else if(irq & 0x8000) {
    // Correct Transfer
    handle_ctr(&g_usb_ctrl, irq);
  } else if(irq & 0x800) {
    // Suspend
    reg_wr(USB_ISTR, ~0x800);
    reg_set_bit(USB_CNTR, 3);
    reg_set_bit(USB_CNTR, 2);
  } else if(irq & 0x1000) {
    // Wakeup
    reg_wr(USB_ISTR, ~0x1000);
    reg_clr_bit(USB_CNTR, 2);
    reg_clr_bit(USB_CNTR, 3);
  }
}


static void
usb_print_info(struct device *d, struct stream *st)
{
  struct usb_ctrl *uc = (struct usb_ctrl *)d;

  const uint16_t cntr = reg_rd(USB_CNTR);
  stprintf(st, "\tFrontend: %s\n",
           (cntr & 2) ? "Off" : "On");

  const uint16_t fnr = reg_rd(USB_FNR);
  stprintf(st, "\tRXDP:%d RXDM:%d Locked:%d Lost SOF:%d Frame:%d\n",
           (fnr >> 15) & 1,
           (fnr >> 14) & 1,
           (fnr >> 13) & 1,
           (fnr >> 11) & 3,
           (fnr >> 0) & 0x7ff);
  const uint16_t daddr = reg_rd(USB_DADDR);
  stprintf(st, "\tEnable: %d  Assigned Address: %d\n",
           (daddr >> 7) & 1,
           daddr & 0x7f);

  stprintf(st, "\tIN Endpoints (TX)\n");
  for(size_t i = 0; i < MAX_NUM_ENDPOINTS; i++) {
    const uint16_t epnr = reg_rd(USB_EPnR(i));

    stprintf(st, "\t\t%d %-9s %-5s IRQ:%d Dtog:%d | ",
             i,
             typestr(epnr),
             statbits((epnr >> 4) & 3),
             (epnr >> 7) & 1,
             (epnr >> 6) & 1);

    usb_ep_t *ue = uc->uc_ue[0][i];
    if(ue != NULL) {
      stprintf(st, "%-10u %-10u %s",
               ue->ue_num_packets,
               ue->ue_num_drops,
               ue->ue_name);
      ue->ue_num_packets = 0;
      ue->ue_num_drops = 0;
    }
    stprintf(st, "\n");
  }

  stprintf(st, "\n\tOUT Endpoints (RX)\n");
  for(size_t i = 0; i < MAX_NUM_ENDPOINTS; i++) {
    const uint16_t epnr = reg_rd(USB_EPnR(i));

    stprintf(st, "\t\t%d %-9s %-5s Irq:%d Dtog:%d | ",
             i,
             typestr(epnr),
             statbits((epnr >> 12) & 3),
             (epnr >> 15) & 1,
             (epnr >> 14) & 1);

    usb_ep_t *ue = uc->uc_ue[1][i];
    if(ue != NULL) {
      stprintf(st, "%-10u %-10u %s",
               ue->ue_num_packets,
               ue->ue_num_drops,
               ue->ue_name);
      ue->ue_num_packets = 0;
      ue->ue_num_drops = 0;
    }

    stprintf(st, "\n");
  }
}


static void
usb_power_state(struct device *dev, device_power_state_t state)
{
  //  struct usb_ctrl *uc = (struct usb_ctrl *)dev;
  if(state == DEVICE_POWER_STATE_RESUME) {
    clk_enable_hsi48();
  }
}



static const device_class_t stm32g4_otgfs_class = {
  .dc_print_info = usb_print_info,
  .dc_power_state = usb_power_state,
};


static void
stm32g4_ep_read(device_t *dev, usb_ep_t *ue,
                uint8_t *buf, size_t buf_size,
                size_t buf_offset, size_t bytes)
{
  const uint32_t ea = ue->ue_address & 0x7f;
  usb_ctrl_t *uc = (usb_ctrl_t *)dev;
  const uint8_t *src = uc->rxbuf[ea];

  for(size_t i = 0; i < bytes; i++) {
    buf[(buf_offset + i) & (buf_size - 1)] = src[i];
  }
}

static int
stm32g4_ep_avail_bytes(device_t *dev, usb_ep_t *ue)
{
  const uint32_t ea = ue->ue_address & 0x7f;

  uint16_t epnr = reg_rd16(USB_EPnR(ea));
  uint16_t state = (epnr >> 4) & 3;
  // If TX is in NAK state (means we've nothing to send) -> say
  // buffer have capacity available
  return state == 2 ? ue->ue_max_packet_size : 0;
}



static error_t
stm32g4_ep_write(device_t *dev, usb_ep_t *ue,
                 const uint8_t *buf, size_t len)
{
  if(stm32g4_ep_avail_bytes(dev, ue) < len)
    return ERR_NOT_READY;

  usb_ctrl_t *uc = (usb_ctrl_t *)dev;
  const uint32_t ea = ue->ue_address & 0x7f;
  reg_wr16(USB_COUNT_TX(ea), len);

  uint16_t *dst16 = (uint16_t *)uc->txbuf[ea];

  while(len > 1) {
    uint8_t a = *buf++;
    uint8_t b = *buf++;
    uint16_t u16 = (b << 8) | a;
    *dst16++ = u16;
    len -= 2;
  }

  if(len) {
    uint8_t a = *buf++;
    *dst16++ = a;
  }

  write_epnr(ea, uc->uc_epreg[ea] | (1 << 4)); // TX: NAK -> VALID
  return 0;
}

static void
stm32g4_ep_write1(device_t *dev, usb_ep_t *ue,
                  size_t len, uint8_t (*getu8)(void *opaque), void *opaque)
{
  usb_ctrl_t *uc = (usb_ctrl_t *)dev;
  const uint32_t ea = ue->ue_address & 0x7f;
  reg_wr16(USB_COUNT_TX(ea), len);

  uint16_t *dst16 = (uint16_t *)uc->txbuf[ea];

  while(len > 1) {
    uint8_t a = getu8(opaque);
    uint8_t b = getu8(opaque);
    uint16_t u16 = (b << 8) | a;
    *dst16++ = u16;
    len -= 2;
  }

  if(len) {
    uint8_t a = getu8(opaque);
    *dst16++ = a;
  }

  write_epnr(ea, uc->uc_epreg[ea] | (1 << 4)); // TX: NAK -> VALID
}

static void
stm32g4_ep_cnak(device_t *dev, usb_ep_t *ue)
{
  const uint32_t ea = ue->ue_address & 0x7f;
  usb_ctrl_t *uc = (usb_ctrl_t *)dev;
  write_epnr(ea, uc->uc_epreg[ea] | (1 << 12)); // RX: NAK -> VALID
}


static const usb_ctrl_vtable_t stm32g4_otgfs_vtable =
{
  .read = stm32g4_ep_read,
  .write = stm32g4_ep_write,
  .write1 = stm32g4_ep_write1,
  .cnak = stm32g4_ep_cnak,
  .avail_bytes = stm32g4_ep_avail_bytes,
};


static int
get_ep_type(usb_ctrl_t *uc, int index)
{
  return (uc->uc_epreg[index] >> 9) & 3;
}

static void
set_ep_type(usb_ctrl_t *uc, int index, int type)
{
  uc->uc_epreg[index] = (1 << 15) | (1 << 7) | (type << 9) | index;
}


static int
alloc_ep(usb_ctrl_t *uc, int out, usb_ep_t *ep)
{
  static const uint8_t type_map[4] = {
    [USB_ENDPOINT_CONTROL]    = EP_TYPE_CONTROL,
    [USB_ENDPOINT_BULK]       = EP_TYPE_BULK,
    [USB_ENDPOINT_ISOCHRONUS] = EP_TYPE_ISO,
    [USB_ENDPOINT_INTERRUPT]  = EP_TYPE_INTERRUPT
  };

  const int type = type_map[ep->ue_endpoint_type];
  for(int i = 0; i < MAX_NUM_ENDPOINTS; i++) {
    if(uc->uc_ue[out][i] != NULL)
      continue;

    if(uc->uc_ue[!out][i] == NULL || get_ep_type(uc, i) == type) {
      set_ep_type(uc, i, type);
      uc->uc_ue[out][i] = ep;
      return i;
    }
  }
  panic("usb: Out of endpoints");
}



void
init_interfaces(usb_ctrl_t *uc)
{
  size_t total_desc_size = sizeof(struct usb_config_descriptor);

  usb_interface_t *ui;
  int num_interfaces = 0;

  uint32_t sramptr = 64; // First 64 bytes is the BTABLE

  // Buffers for EP0
  reg_wr16(USB_ADDR_TX(0), sramptr);
  uc->txbuf[0] = (uint8_t *)(intptr_t)USB_SRAM + sramptr;
  sramptr += 8;
  reg_wr16(USB_COUNT_TX(0), 0);
  uc->rxbuf[0] = (uint8_t *)(intptr_t)USB_SRAM + sramptr;
  reg_wr16(USB_ADDR_RX(0), sramptr);
  sramptr += 8;
  reg_wr16(USB_COUNT_RX(0), (4 << 10)); // 8 byte buffer

  STAILQ_FOREACH(ui, &uc->uc_ifaces, ui_link) {
    total_desc_size += ui->ui_gen_desc(NULL, ui->ui_opaque, 0);
    total_desc_size +=
      ui->ui_num_endpoints * sizeof(struct usb_endpoint_descriptor);
    num_interfaces++;
  }

  uc->uc_ud.ud_config_desc_size = total_desc_size;
  uc->uc_ud.ud_config_desc = calloc(1, total_desc_size);
  void *o = uc->uc_ud.ud_config_desc;

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
    ui->ui_index = iface_index;

    iface_index++;

    for(size_t j = 0; j < ui->ui_num_endpoints; j++)  {
      usb_ep_t *ue = ui->ui_endpoints + j;
      assert(ue->ue_max_packet_size < 2048);
      ue->ue_dev = &uc->uc_ud.ud_dev;
      ue->ue_vtable = &stm32g4_otgfs_vtable;
      ue->ue_name = ui->ui_name;

      const int out = !(ue->ue_address & 0x80);
      const int ea = alloc_ep(uc, out, ue);
      ue->ue_address |= ea;

      struct usb_endpoint_descriptor *ued = o;
      ued->bLength = sizeof(struct usb_endpoint_descriptor);
      ued->bDescriptorType = USB_DESC_TYPE_ENDPOINT;
      ued->bEndpointAddress = ue->ue_address;
      ued->bmAttributes = ue->ue_endpoint_type;
      ued->wMaxPacketSize = ue->ue_max_packet_size;
      ued->bInterval = ue->ue_interval;

      if(out) {
        // RX
        uc->rxbuf[ea] = (uint8_t *)(intptr_t)USB_SRAM + sramptr;
        reg_wr16(USB_ADDR_RX(ea), sramptr);
        int size = ue->ue_max_packet_size;
        if(size > 62) {
          int num_block = (size + 31) / 32;
          reg_wr16(USB_COUNT_RX(ea), 0x8000 | ((num_block - 1) << 10));
          size = num_block * 32;
        } else {
          reg_wr16(USB_COUNT_RX(ea), (size << 10));
        }
        sramptr += size;
      } else {
        uc->txbuf[ea] = (uint8_t *)(intptr_t)USB_SRAM + sramptr;
        reg_wr16(USB_ADDR_TX(ea), sramptr);
        sramptr += ue->ue_max_packet_size;
      }

      o += sizeof(struct usb_endpoint_descriptor);
    }
  }

  assert(o == uc->uc_ud.ud_config_desc + uc->uc_ud.ud_config_desc_size);
}



void
stm32g4_usb_create(uint16_t vid, uint16_t pid,
                   const char *manfacturer_string,
                   const char *product_string,
                   struct usb_interface_queue *q)
{
  usb_ctrl_t *uc = &g_usb_ctrl;
  uc->uc_ifaces = *q;
  STAILQ_INIT(q);

  uc->uc_ud.ud_dev.d_class = &stm32g4_otgfs_class;
  uc->uc_ud.ud_dev.d_name = "usb";
  device_register(&uc->uc_ud.ud_dev);

  uc->uc_ud.ud_manufacturer = manfacturer_string;
  uc->uc_ud.ud_product = product_string;
  uc->uc_ud.ud_desc.idVendor = vid;
  uc->uc_ud.ud_desc.idProduct = pid;

  clk_enable(CLK_USB);
  clk_enable_hsi48();

  for(int i = 0; i < 1024; i += 2) {
    reg_wr16(USB_SRAM + i, 0);
  }

  init_interfaces(uc);

  reg_wr(USB_CNTR, 1);
  udelay(1);

  reg_wr(USB_CNTR, 0x9c01);
  reg_wr(USB_BCDR, 0x8000);
  reg_wr(USB_CNTR, 0x9c00);

  irq_enable(19, IRQ_LEVEL_NET);
  irq_enable(20, IRQ_LEVEL_NET);
}


void
stm32g4_usb_disable(int yes)
{
  if(yes) {
    reg_set_bit(USB_CNTR, 1);
  } else {
    reg_clr_bit(USB_CNTR, 1);
  }
}

void
stm32g4_usb_stop(void)
{
  reg_wr(USB_BCDR, 0);
  reg_wr(USB_ISTR, 0);
  reg_wr(USB_DADDR, 0);
  reg_wr(USB_CNTR, 3);
}
