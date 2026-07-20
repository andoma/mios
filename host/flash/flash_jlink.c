#include "flash.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "jlink/jlink.h"
#include "jlink/dap.h"
#include "jlink/nrf54l.h"
#include "mios_image.h"

#define RRAM_SIZE 0x180000

static double
now(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec * 1e-6;
}

int
flash_jlink(const flash_params_t *p, flash_log_t *log)
{
  mios_image_t *mi = NULL;
  jlink_t *jl = NULL;
  dap_t *d = NULL;
  uint8_t *rb = NULL;
  int retcode = -1;

  if(!(p->flags & FLASH_RESET_ONLY)) {
    const char *errmsg;
    mi = mios_image_from_elf_file(p->elf_path, 0, 4, &errmsg);
    if(mi == NULL) {
      flash_logf(log, "%s: %s", p->elf_path, errmsg);
      return -1;
    }
    if(mi->load_addr + mi->image_size > RRAM_SIZE) {
      flash_logf(log, "Image at 0x%08llx + %zd bytes does not fit in RRAM",
                 (long long)mi->load_addr, mi->image_size);
      goto out;
    }
    flash_logf(log, "%s: %zd bytes at 0x%08llx",
               mi->appname, mi->image_size, (long long)mi->load_addr);
  }

  jl = jlink_open(p->serial);
  if(jl == NULL) {
    flash_logf(log, "No J-Link probe found");
    goto out;
  }

  flash_logf(log, "Probe: %s (S/N %s), target voltage %dmV",
             jlink_version(jl), jlink_serial(jl), jlink_target_voltage(jl));

  d = dap_create(jl);

  if(jlink_select_swd(jl) ||
     jlink_set_speed_khz(jl, p->swd_khz ? p->swd_khz : 4000)) {
    flash_logf(log, "%s", jlink_errmsg(jl));
    goto out;
  }

  uint32_t dpidr;
  if(dap_connect(d, &dpidr)) {
    flash_logf(log, "%s", dap_errmsg(d));
    goto out;
  }
  flash_logf(log, "DPIDR: 0x%08x", dpidr);

  if(p->flags & FLASH_RECOVER) {
    flash_logf(log, "Recovering (erase-all via CTRL-AP)...");
    if(nrf54l_recover(d)) {
      flash_logf(log, "Recover failed: %s", dap_errmsg(d));
      goto out;
    }
    // The recover reset drops debug power-up; reconnect
    if(dap_connect(d, &dpidr)) {
      flash_logf(log, "%s", dap_errmsg(d));
      goto out;
    }
  }

  int device_en;
  if(dap_mem_init(d, &device_en)) {
    flash_logf(log, "%s", dap_errmsg(d));
    goto out;
  }
  if(!device_en) {
    flash_logf(log, "Debug access is locked (approtect). "
               "Recover (erase-all) to unlock.");
    goto out;
  }

  if(p->flags & FLASH_RESET_ONLY) {
    if(nrf54l_reset_run(d)) {
      flash_logf(log, "Reset failed: %s", dap_errmsg(d));
      goto out;
    }
    flash_logf(log, "Target reset");
    retcode = 0;
    goto out;
  }

  char ident[64];
  if(nrf54l_identify(d, ident, sizeof(ident)) < 0) {
    flash_logf(log, "Failed to read FICR: %s", dap_errmsg(d));
    goto out;
  }
  flash_logf(log, "Target: %s", ident);

  if(nrf54l_reset_halt(d)) {
    flash_logf(log, "Failed to reset+halt: %s", dap_errmsg(d));
    goto out;
  }

  double t0 = now();
  if(nrf54l_program(d, mi->load_addr, mi->image, mi->image_size)) {
    flash_logf(log, "Programming failed: %s", dap_errmsg(d));
    goto out;
  }
  double dt = now() - t0;
  flash_logf(log, "Programmed %zd bytes in %.2fs (%.0f kB/s)",
             mi->image_size, dt, mi->image_size / dt / 1024);

  if(!(p->flags & FLASH_NO_VERIFY)) {
    rb = malloc(mi->image_size);
    t0 = now();
    if(dap_mem_read_block(d, mi->load_addr, rb, mi->image_size)) {
      flash_logf(log, "Readback failed: %s", dap_errmsg(d));
      goto out;
    }
    dt = now() - t0;
    if(memcmp(rb, mi->image, mi->image_size)) {
      for(size_t i = 0; i < mi->image_size; i++) {
        if(rb[i] != mi->image[i]) {
          flash_logf(log,
                     "Verify FAILED at 0x%08llx (got 0x%02x, expected 0x%02x)",
                     (long long)mi->load_addr + i, rb[i], mi->image[i]);
          break;
        }
      }
      goto out;
    }
    flash_logf(log, "Verified in %.2fs (%.0f kB/s)",
               dt, mi->image_size / dt / 1024);
  }

  if(!(p->flags & FLASH_NO_RUN)) {
    if(nrf54l_reset_run(d)) {
      flash_logf(log, "Reset failed: %s", dap_errmsg(d));
      goto out;
    }
    flash_logf(log, "Target running");
  }
  retcode = 0;

out:
  free(rb);
  dap_destroy(d);
  jlink_close(jl);
  free(mi);
  return retcode;
}
