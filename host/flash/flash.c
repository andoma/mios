#include "flash.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>

void
flash_log_init(flash_log_t *l, FILE *tee)
{
  l->cap = 4096;
  l->len = 0;
  l->buf = malloc(l->cap);
  l->buf[0] = 0;
  l->tee = tee;
}

void
flash_log_free(flash_log_t *l)
{
  free(l->buf);
  l->buf = NULL;
}

void
flash_logf(flash_log_t *l, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char *line = NULL;
  if(vasprintf(&line, fmt, ap) < 0) {
    va_end(ap);
    return;
  }
  va_end(ap);

  const size_t n = strlen(line);
  if(l->len + n + 2 > l->cap) {
    while(l->len + n + 2 > l->cap)
      l->cap *= 2;
    l->buf = realloc(l->buf, l->cap);
  }
  memcpy(l->buf + l->len, line, n);
  l->len += n;
  l->buf[l->len++] = '\n';
  l->buf[l->len] = 0;

  if(l->tee != NULL) {
    fprintf(l->tee, "%s\n", line);
    fflush(l->tee);
  }
  free(line);
}

// Is a J-Link probe present on USB?
static int
jlink_probe_present(void)
{
  libusb_context *ctx;
  if(libusb_init(&ctx))
    return 0;

  libusb_device **list;
  const ssize_t num = libusb_get_device_list(ctx, &list);
  int found = 0;
  for(ssize_t i = 0; i < num && !found; i++) {
    struct libusb_device_descriptor desc;
    if(libusb_get_device_descriptor(list[i], &desc) == 0 &&
       desc.idVendor == 0x1366)
      found = 1;
  }
  libusb_free_device_list(list, 1);
  libusb_exit(ctx);
  return found;
}

int
flash_run(const flash_params_t *p, flash_log_t *log)
{
  const char *method = p->method;

  if(method == NULL || !strcmp(method, "auto")) {
    // J-Link detection is passive; DFU probing may detach a runtime
    // device into its bootloader so it goes last of the USB methods
    if(jlink_probe_present())
      method = "jlink";
    else
      method = "dfu";
    flash_logf(log, "Auto-selected method: %s", method);
  }

  if(!strcmp(method, "jlink"))
    return flash_jlink(p, log);
  if(!strcmp(method, "dfu"))
    return flash_dfu(p, log);
  if(!strcmp(method, "openocd"))
    return flash_openocd(p, log);
  if(!strcmp(method, "nrfdfu"))
    return flash_nrfdfu(p, log);

  flash_logf(log, "Unknown flash method '%s'", method);
  return -1;
}
