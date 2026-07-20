#include "../dfu.c" // Written to be #included; provides dfu_flash_elf()

#include "flash.h"

static void
dfu_to_flash_log(void *opaque, int level, const char *msg)
{
  (void)level;
  flash_logf(opaque, "%s", msg);
}

int
flash_dfu(const flash_params_t *p, flash_log_t *log)
{
  if(p->flags & FLASH_RESET_ONLY) {
    flash_logf(log, "Reset-only is not supported for DFU");
    return -1;
  }

  libusb_context *usb;
  if(libusb_init(&usb)) {
    flash_logf(log, "libusb_init failed");
    return -1;
  }

  dfu_set_logger(dfu_to_flash_log, log);

  const char *err = dfu_flash_elf(usb, p->elf_path,
                                  !!(p->flags & FLASH_FORCE), p->cmdline);
  dfu_set_logger(NULL, NULL);
  libusb_exit(usb);

  if(err != NULL) {
    flash_logf(log, "%s", err);
    return -1;
  }
  flash_logf(log, "DFU flash successful");
  return 0;
}
