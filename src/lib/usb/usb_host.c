#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mios/device.h>
#include <mios/error.h>
#include <mios/stream.h>

#include <usb/usb_host.h>

// Only the root port for now, so the address can be fixed
#define USB_HOST_ROOT_ADDR 1

static usb_host_driver_t *usb_host_drivers;

void
usb_host_driver_register(usb_host_driver_t *uhd)
{
  uhd->uhd_next = usb_host_drivers;
  usb_host_drivers = uhd;
}

int
usb_host_control(usb_host_device_t *ud, uint8_t request_type,
                 uint8_t request, uint16_t value, uint16_t index,
                 void *data, uint16_t length)
{
  const struct usb_setup_packet setup = {
    .request_type = request_type,
    .request = request,
    .value = value,
    .index = index,
    .length = length
  };
  usb_host_ctrl_t *uhc = ud->ud_uhc;
  return uhc->uhc_vtable->control(uhc, ud->ud_addr, ud->ud_speed,
                                  ud->ud_mps0, &setup, data);
}


usb_host_pipe_t *
usb_host_pipe_open(usb_host_device_t *ud,
                   const struct usb_endpoint_descriptor *ed)
{
  usb_host_ctrl_t *uhc = ud->ud_uhc;
  return uhc->uhc_vtable->pipe_open(uhc, ud->ud_addr, ud->ud_speed, ed);
}


void
usb_host_pipe_close(usb_host_device_t *ud, usb_host_pipe_t *up)
{
  usb_host_ctrl_t *uhc = ud->ud_uhc;
  uhc->uhc_vtable->pipe_close(uhc, up);
}


error_t
usb_host_submit(usb_host_device_t *ud, usb_host_pipe_t *up,
                usb_host_xfer_t *ux)
{
  usb_host_ctrl_t *uhc = ud->ud_uhc;
  return uhc->uhc_vtable->submit(uhc, up, ux);
}


// Step to the next descriptor, NULL at the end or if malformed
static const uint8_t *
desc_next(usb_host_device_t *ud, const uint8_t *p)
{
  const uint8_t *end = ud->ud_config + ud->ud_config_len;
  p += p[0];
  if(p + 2 > end || p[0] < 2 || p + p[0] > end)
    return NULL;
  return p;
}


const struct usb_interface_descriptor *
usb_host_find_interface(usb_host_device_t *ud, int ifnum)
{
  for(const uint8_t *p = ud->ud_config; p != NULL; p = desc_next(ud, p)) {
    if(p[1] != USB_DESC_TYPE_INTERFACE ||
       p[0] < sizeof(struct usb_interface_descriptor))
      continue;
    const struct usb_interface_descriptor *id = (const void *)p;
    if(id->bInterfaceNumber == ifnum && id->bAlternateSetting == 0)
      return id;
  }
  return NULL;
}


const void *
usb_host_find_desc(usb_host_device_t *ud, const void *from, int type)
{
  const uint8_t *p = from;
  while((p = desc_next(ud, p)) != NULL) {
    if(p[1] == USB_DESC_TYPE_INTERFACE)
      return NULL;
    if(p[1] == type)
      return p;
  }
  return NULL;
}


const struct usb_endpoint_descriptor *
usb_host_find_endpoint(usb_host_device_t *ud,
                       const struct usb_interface_descriptor *id,
                       int type, int dir)
{
  const struct usb_endpoint_descriptor *ed = (const void *)id;
  while((ed = usb_host_find_desc(ud, ed, USB_DESC_TYPE_ENDPOINT)) != NULL) {
    if(ed->bLength >= sizeof(struct usb_endpoint_descriptor) &&
       (ed->bmAttributes & 3) == type &&
       (ed->bEndpointAddress & 0x80) == dir)
      return ed;
  }
  return NULL;
}


static void
bind_drivers(usb_host_device_t *ud)
{
  int num_bindings = 0;

  for(const uint8_t *p = ud->ud_config; p != NULL; p = desc_next(ud, p)) {
    if(p[1] != USB_DESC_TYPE_INTERFACE ||
       p[0] < sizeof(struct usb_interface_descriptor))
      continue;
    const struct usb_interface_descriptor *id = (const void *)p;
    if(id->bAlternateSetting != 0 || id->bInterfaceNumber >= 32 ||
       ud->ud_claimed & (1 << id->bInterfaceNumber))
      continue;

    for(usb_host_driver_t *uhd = usb_host_drivers; uhd != NULL;
        uhd = uhd->uhd_next) {
      if(num_bindings == USB_HOST_MAX_BINDINGS)
        return;
      void *instance = uhd->uhd_probe(ud, id);
      if(instance == NULL)
        continue;
      ud->ud_claimed |= 1 << id->bInterfaceNumber;
      ud->ud_bindings[num_bindings].driver = uhd;
      ud->ud_bindings[num_bindings].instance = instance;
      num_bindings++;
      break;
    }
  }
}


static int
get_descriptor(usb_host_device_t *ud, uint8_t type, uint8_t index,
               uint16_t langid, void *buf, uint16_t len)
{
  return usb_host_control(ud, 0x80, USB_REQ_GET_DESCRIPTOR,
                          (type << 8) | index, langid, buf, len);
}


// Fetch a string descriptor and convert it to ASCII
static char *
get_string(usb_host_device_t *ud, uint8_t index, uint16_t langid)
{
  if(index == 0)
    return NULL;

  uint8_t *buf = malloc(256);
  char *s = NULL;
  int r = get_descriptor(ud, USB_DESC_TYPE_STRING, index, langid, buf, 255);
  if(r >= 2 && buf[1] == USB_DESC_TYPE_STRING) {
    const int len = r < buf[0] ? r : buf[0];
    const int chars = (len - 2) / 2;
    s = malloc(chars + 1);
    for(int i = 0; i < chars; i++) {
      const uint16_t c = buf[2 + i * 2] | (buf[3 + i * 2] << 8);
      s[i] = c >= 0x20 && c < 0x7f ? c : '?';
    }
    s[chars] = 0;
  }
  free(buf);
  return s;
}


static error_t
enumerate(usb_host_device_t *ud)
{
  struct usb_device_descriptor *dd = &ud->ud_desc;

  // Address 0 and a conservative EP0 packet size until we know better.
  // The first 8 bytes of the device descriptor contain bMaxPacketSize0
  int r = get_descriptor(ud, USB_DESC_TYPE_DEVICE, 0, 0, dd, 8);
  if(r < 0)
    return r;
  if(r < 8 || dd->bMaxPacketSize0 < 8)
    return ERR_MALFORMED;
  ud->ud_mps0 = dd->bMaxPacketSize0;

  r = usb_host_control(ud, 0, USB_REQ_SET_ADDRESS, USB_HOST_ROOT_ADDR, 0,
                       NULL, 0);
  if(r < 0)
    return r;
  ud->ud_addr = USB_HOST_ROOT_ADDR;
  usleep(10000); // Spec says 2ms recovery after SET_ADDRESS, be generous

  r = get_descriptor(ud, USB_DESC_TYPE_DEVICE, 0, 0, dd, sizeof(*dd));
  if(r < 0)
    return r;
  if(r < (int)sizeof(*dd))
    return ERR_MALFORMED;

  struct usb_config_descriptor cd;
  r = get_descriptor(ud, USB_DESC_TYPE_CONFIGURATION, 0, 0, &cd, sizeof(cd));
  if(r < 0)
    return r;
  if(r < (int)sizeof(cd) || cd.wTotalLength < sizeof(cd))
    return ERR_MALFORMED;

  ud->ud_config_len = cd.wTotalLength;
  ud->ud_config = malloc(ud->ud_config_len);
  r = get_descriptor(ud, USB_DESC_TYPE_CONFIGURATION, 0, 0,
                     ud->ud_config, ud->ud_config_len);
  if(r < 0)
    return r;
  if(r != ud->ud_config_len)
    return ERR_MALFORMED;

  // String descriptor 0 holds the supported language ids
  uint16_t langid = 0x409;
  uint8_t langs[4];
  r = get_descriptor(ud, USB_DESC_TYPE_STRING, 0, 0, langs, sizeof(langs));
  if(r == sizeof(langs))
    langid = langs[2] | (langs[3] << 8);

  ud->ud_manufacturer = get_string(ud, dd->iManufacturer, langid);
  ud->ud_product = get_string(ud, dd->iProduct, langid);
  ud->ud_serial = get_string(ud, dd->iSerialNumber, langid);

  r = usb_host_control(ud, 0, USB_REQ_SET_CONFIG, cd.bConfigurationValue, 0,
                       NULL, 0);
  if(r < 0)
    return r;
  return 0;
}


static const char *
speed_str(int speed)
{
  switch(speed) {
  case USB_SPEED_HIGH:
    return "high";
  case USB_SPEED_FULL:
    return "full";
  case USB_SPEED_LOW:
    return "low";
  default:
    return "unknown";
  }
}


static const char *
ep_type_str(int type)
{
  static const char *types[4] = {"control", "isochronous", "bulk", "interrupt"};
  return types[type & 3];
}


static void
print_config(const uint8_t *p, size_t len, struct stream *st)
{
  const uint8_t *end = p + len;

  while(p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
    const int dlen = p[0];

    switch(p[1]) {
    case USB_DESC_TYPE_CONFIGURATION:
      if(dlen < sizeof(struct usb_config_descriptor))
        break;
      const struct usb_config_descriptor *cd = (const void *)p;
      stprintf(st, "Configuration %d: %d interfaces, %s powered, %d mA\n",
               cd->bConfigurationValue, cd->bNumInterfaces,
               cd->bmAttributes & 0x40 ? "self" : "bus",
               cd->bMaxPower * 2);
      break;

    case USB_DESC_TYPE_INTERFACE_ASSOC:
      if(dlen < sizeof(struct usb_iad_descriptor))
        break;
      const struct usb_iad_descriptor *iad = (const void *)p;
      stprintf(st, "  Function: interfaces %d-%d class %02x/%02x/%02x\n",
               iad->bFirstInterface,
               iad->bFirstInterface + iad->bInterfaceCount - 1,
               iad->bFunctionClass, iad->bFunctionSubClass,
               iad->bFunctionProtocol);
      break;

    case USB_DESC_TYPE_INTERFACE:
      if(dlen < sizeof(struct usb_interface_descriptor))
        break;
      const struct usb_interface_descriptor *id = (const void *)p;
      stprintf(st, "  Interface %d alt %d: class %02x/%02x/%02x, "
               "%d endpoints\n",
               id->bInterfaceNumber, id->bAlternateSetting,
               id->bInterfaceClass, id->bInterfaceSubClass,
               id->bInterfaceProtocol, id->bNumEndpoints);
      break;

    case USB_DESC_TYPE_ENDPOINT:
      if(dlen < sizeof(struct usb_endpoint_descriptor))
        break;
      const struct usb_endpoint_descriptor *ed = (const void *)p;
      stprintf(st, "    EP 0x%02x %-3s %-11s max packet %4d interval %d\n",
               ed->bEndpointAddress,
               ed->bEndpointAddress & 0x80 ? "IN" : "OUT",
               ep_type_str(ed->bmAttributes),
               ed->wMaxPacketSize & 0x7ff, ed->bInterval);
      break;

    case USB_DESC_TYPE_CS_INTERFACE:
      stprintf(st, "    Class specific: subtype 0x%02x, %d bytes\n",
               dlen >= 3 ? p[2] : 0, dlen);
      break;

    default:
      stprintf(st, "    Descriptor type 0x%02x, %d bytes\n", p[1], dlen);
      break;
    }
    p += dlen;
  }
}


static void
usb_host_device_print_info(struct device *d, struct stream *st)
{
  const usb_host_device_t *ud = (const usb_host_device_t *)d;
  const struct usb_device_descriptor *dd = &ud->ud_desc;

  stprintf(st, "%04x:%04x  USB %x.%02x  %s speed  address %d\n",
           dd->idVendor, dd->idProduct,
           dd->bcdUSB >> 8, dd->bcdUSB & 0xff,
           speed_str(ud->ud_speed), ud->ud_addr);
  stprintf(st, "Manufacturer: %s\n",
           ud->ud_manufacturer ?: "");
  stprintf(st, "Product: %s\n",
           ud->ud_product ?: "");
  stprintf(st, "Serial: %s\n",
           ud->ud_serial ?: "");
  stprintf(st, "Class %02x/%02x/%02x  EP0 max packet %d\n",
           dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol,
           ud->ud_mps0);
  print_config(ud->ud_config, ud->ud_config_len, st);
}


static void
usb_host_device_dtor(struct device *d)
{
  usb_host_device_t *ud = (usb_host_device_t *)d;
  free(ud->ud_config);
  free(ud->ud_manufacturer);
  free(ud->ud_product);
  free(ud->ud_serial);
  free(ud);
}


static const device_class_t usb_host_device_class = {
  .dc_class_name = "usb-device",
  .dc_print_info = usb_host_device_print_info,
  .dc_dtor = usb_host_device_dtor,
};


void
usb_host_attach(usb_host_ctrl_t *uhc, int speed)
{
  usb_host_device_t *ud = calloc(1, sizeof(usb_host_device_t));
  ud->ud_uhc = uhc;
  ud->ud_speed = speed;
  ud->ud_mps0 = speed == USB_SPEED_LOW ? 8 : 64;

  error_t err = enumerate(ud);
  if(err) {
    printf("%s: Enumeration failed: %s\n", uhc->uhc_dev.d_name,
           error_to_string(err));
    usb_host_device_dtor(&ud->ud_dev);
    return;
  }

  snprintf(ud->ud_name, sizeof(ud->ud_name), "usbdev%d", ud->ud_addr);
  ud->ud_dev.d_name = ud->ud_name;
  ud->ud_dev.d_class = &usb_host_device_class;
  ud->ud_dev.d_parent = &uhc->uhc_dev;
  device_register(&ud->ud_dev);
  uhc->uhc_root = ud;

  printf("%s: %04x:%04x %s %s (%s speed)\n", uhc->uhc_dev.d_name,
         ud->ud_desc.idVendor, ud->ud_desc.idProduct,
         ud->ud_manufacturer ?: "", ud->ud_product ?: "",
         speed_str(speed));

  bind_drivers(ud);
}


void
usb_host_detach(usb_host_ctrl_t *uhc)
{
  usb_host_device_t *ud = uhc->uhc_root;
  if(ud == NULL)
    return;
  uhc->uhc_root = NULL;
  printf("%s: %04x:%04x disconnected\n", uhc->uhc_dev.d_name,
         ud->ud_desc.idVendor, ud->ud_desc.idProduct);

  for(int i = 0; i < USB_HOST_MAX_BINDINGS; i++) {
    if(ud->ud_bindings[i].driver != NULL)
      ud->ud_bindings[i].driver->uhd_detach(ud, ud->ud_bindings[i].instance);
  }
  device_unregister(&ud->ud_dev);
}
