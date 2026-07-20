// Flash via a running OpenOCD instance's TCL command interface
// (typically an ST-Link attached to an STM32)

#include "flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include "mios_image.h"

#define OPENOCD_TCL_PORT 6666

// STM32 flash starts at 0x08000000. If the load address is elsewhere
// (RAM, e.g. STM32N6) we use load_image instead of program.
#define STM32_FLASH_BASE 0x08000000

static int
openocd_connect(const char *host, int port)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0)
    return -1;

  struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
  };
  inet_pton(AF_INET, host, &addr.sin_addr);

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// OpenOCD TCL protocol: command and response are terminated by SUB (0x1a).
// Returns allocated response string, or NULL on error.
static char *
openocd_command(int fd, const char *cmd, int timeout_ms)
{
  size_t cmdlen = strlen(cmd);
  char *sendbuf = malloc(cmdlen + 1);
  memcpy(sendbuf, cmd, cmdlen);
  sendbuf[cmdlen] = 0x1a;

  ssize_t w = write(fd, sendbuf, cmdlen + 1);
  free(sendbuf);
  if(w < 0)
    return NULL;

  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);

  while(1) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if(r <= 0)
      break;

    ssize_t n = read(fd, buf + len, cap - len - 1);
    if(n <= 0)
      break;

    len += n;
    buf[len] = '\0';

    if(len > 0 && buf[len - 1] == 0x1a) {
      buf[len - 1] = '\0';
      len--;
      break;
    }

    if(len + 256 >= cap) {
      cap *= 2;
      buf = realloc(buf, cap);
    }
  }

  return buf;
}

// Send a command, log it, and check for errors. Returns 0 on success.
static int
openocd_cmd(int fd, const char *cmd, int timeout_ms, flash_log_t *log)
{
  flash_logf(log, "> %s", cmd);
  char *resp = openocd_command(fd, cmd, timeout_ms);
  if(resp == NULL) {
    flash_logf(log, "No response from OpenOCD (timeout?)");
    return -1;
  }
  if(resp[0])
    flash_logf(log, "%s", resp);

  const int err = strstr(resp, "Error") || strstr(resp, "error");
  free(resp);
  return err ? -1 : 0;
}

static int
is_ram_target(uint64_t load_addr)
{
  return (load_addr >> 28) != (STM32_FLASH_BASE >> 28);
}

int
flash_openocd(const flash_params_t *p, flash_log_t *log)
{
  const char *host = p->openocd_host ? p->openocd_host : "127.0.0.1";
  const int port = p->openocd_port ? p->openocd_port : OPENOCD_TCL_PORT;

  int fd = openocd_connect(host, port);
  if(fd < 0) {
    flash_logf(log, "Failed to connect to OpenOCD TCL interface at %s:%d "
               "(is OpenOCD running?)", host, port);
    return -1;
  }

  if(p->flags & FLASH_RESET_ONLY) {
    const int r = openocd_cmd(fd, "reset run", 5000, log);
    close(fd);
    return r;
  }

  // Parse ELF to determine load address (flash vs RAM)
  const char *elf_err;
  mios_image_t *mi = mios_image_from_elf_file(p->elf_path, 0, 0, &elf_err);
  if(mi == NULL) {
    flash_logf(log, "%s: %s", p->elf_path, elf_err);
    close(fd);
    return -1;
  }
  const int ram_target = is_ram_target(mi->load_addr);
  free(mi);

  char cmd[1024];
  int r = -1;

  // Always reset halt first: safe for all targets, required for RAM targets
  if(openocd_cmd(fd, "reset halt", 5000, log))
    goto out;

  if(ram_target) {
    flash_logf(log, "RAM target detected (load addr not in flash)");
    snprintf(cmd, sizeof(cmd), "load_image %s", p->elf_path);
    if(openocd_cmd(fd, cmd, 30000, log))
      goto out;
    if(!(p->flags & FLASH_NO_RUN) && openocd_cmd(fd, "resume", 5000, log))
      goto out;
  } else {
    snprintf(cmd, sizeof(cmd), "program %s%s%s",
             p->elf_path,
             (p->flags & FLASH_NO_VERIFY) ? "" : " verify",
             (p->flags & FLASH_NO_RUN) ? "" : " reset");
    if(openocd_cmd(fd, cmd, 30000, log))
      goto out;
  }

  flash_logf(log, "OpenOCD flash successful");
  r = 0;

out:
  close(fd);
  return r;
}
