#include "mcp_server.h"
#include "mios_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#define OPENOCD_TCL_PORT 6666

// STM32 flash starts at 0x08000000, RAM at 0x20000000.
// If load address is in RAM range, we use load_image instead of program.
#define STM32_FLASH_BASE 0x08000000
#define STM32_RAM_BASE   0x20000000

// Connect to OpenOCD's TCL command interface.
// Returns socket fd or -1 on error.
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

// Send a command to OpenOCD TCL interface and read the response.
// OpenOCD TCL protocol: send command + \x1a, response ends with \x1a.
// Returns allocated response string, or NULL on error.
static char *
openocd_command(int fd, const char *cmd, int timeout_ms)
{
  // Send command terminated by SUB (0x1a)
  size_t cmdlen = strlen(cmd);
  char *sendbuf = malloc(cmdlen + 1);
  memcpy(sendbuf, cmd, cmdlen);
  sendbuf[cmdlen] = 0x1a;

  ssize_t w = write(fd, sendbuf, cmdlen + 1);
  free(sendbuf);
  if(w < 0)
    return NULL;

  // Read response until 0x1a terminator
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

    // Check for SUB terminator
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

// Growable log buffer
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} log_t;

static void
log_init(log_t *l)
{
  l->cap = 4096;
  l->len = 0;
  l->buf = malloc(l->cap);
  l->buf[0] = '\0';
}

static void __attribute__((format(printf, 2, 3)))
log_append(log_t *l, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(l->buf + l->len, l->cap - l->len, fmt, ap);
  va_end(ap);
  if(n > 0)
    l->len += n;
}

// Send a command, log it, and check for errors.
// Returns 0 on success, -1 on error (errstr set).
static int
openocd_cmd(int fd, const char *cmd, int timeout_ms,
            log_t *log, const char **errstr)
{
  log_append(log, "> %s\n", cmd);
  char *resp = openocd_command(fd, cmd, timeout_ms);
  if(!resp) {
    *errstr = "No response from OpenOCD (timeout?)";
    return -1;
  }
  if(resp[0])
    log_append(log, "%s\n", resp);

  if(strstr(resp, "Error") || strstr(resp, "error")) {
    *errstr = resp;  // caller sees error before we return
    return -1;
  }
  free(resp);
  return 0;
}

static int
is_ram_target(uint64_t load_addr)
{
  return (load_addr >> 28) != (STM32_FLASH_BASE >> 28);
}

static cJSON *
tool_flash_stlink(mcp_context_t *ctx, const cJSON *params, const char **errstr)
{
  const cJSON *elf_path = cJSON_GetObjectItem(params, "elf_path");
  if(!cJSON_IsString(elf_path)) {
    *errstr = "Missing required parameter: elf_path";
    return NULL;
  }

  // Parse ELF to determine load address (flash vs RAM)
  const char *elf_err;
  mios_image_t *mi = mios_image_from_elf_file(elf_path->valuestring,
                                               0, 0, &elf_err);
  if(!mi) {
    *errstr = elf_err;
    return NULL;
  }

  int ram_target = is_ram_target(mi->load_addr);
  free(mi);

  const cJSON *host_param = cJSON_GetObjectItem(params, "host");
  const cJSON *port_param = cJSON_GetObjectItem(params, "port");

  const char *host = cJSON_IsString(host_param)
    ? host_param->valuestring : "127.0.0.1";
  int port = cJSON_IsNumber(port_param)
    ? (int)port_param->valuedouble : OPENOCD_TCL_PORT;

  int fd = openocd_connect(host, port);
  if(fd < 0) {
    *errstr = "Failed to connect to OpenOCD TCL interface "
              "(is OpenOCD running?)";
    return NULL;
  }

  log_t log;
  log_init(&log);

  if(ram_target) {
    log_append(&log, "RAM target detected (load addr not in flash)\n\n");
  }

  // Always reset halt first — safe for all targets, required for RAM targets
  if(openocd_cmd(fd, "reset halt", 5000, &log, errstr))
    goto fail;

  if(ram_target) {
    // RAM target: load image directly, then resume
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "load_image %s", elf_path->valuestring);
    if(openocd_cmd(fd, cmd, 30000, &log, errstr))
      goto fail;

    if(openocd_cmd(fd, "resume", 5000, &log, errstr))
      goto fail;

    log_append(&log, "\nRAM load via ST-Link successful.\n");
  } else {
    // Flash target: use program command (handles erase+write+verify)
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "program %s verify reset",
             elf_path->valuestring);
    if(openocd_cmd(fd, cmd, 30000, &log, errstr))
      goto fail;

    log_append(&log, "\nFlash via ST-Link successful.\n");
  }

  close(fd);
  cJSON *result = mcp_text_result(log.buf);
  free(log.buf);
  return result;

fail:
  close(fd);
  free(log.buf);
  return NULL;
}

static cJSON *schema;

void
mcp_tool_stlink_init(mcp_context_t *ctx)
{
  schema = cJSON_Parse(
    "{"
    "  \"type\": \"object\","
    "  \"properties\": {"
    "    \"elf_path\": {"
    "      \"type\": \"string\","
    "      \"description\": \"Path to ELF firmware file to flash\""
    "    },"
    "    \"host\": {"
    "      \"type\": \"string\","
    "      \"description\": \"OpenOCD TCL interface host\","
    "      \"default\": \"127.0.0.1\""
    "    },"
    "    \"port\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"OpenOCD TCL interface port\","
    "      \"default\": 6666"
    "    }"
    "  },"
    "  \"required\": [\"elf_path\"]"
    "}");

  static mcp_tool_t tool = {
    .name = "flash_stlink",
    .description = "Load firmware onto an STM32 device via ST-Link by "
      "connecting to a running OpenOCD instance's TCL command interface "
      "(default port 6666). Auto-detects flash vs RAM targets from the "
      "ELF load address: flash targets use 'program' (erase+write+verify), "
      "RAM targets (e.g. STM32N6) use 'reset halt' + 'load_image' + "
      "'resume'. OpenOCD must already be running.",
    .handler = tool_flash_stlink,
  };
  tool.input_schema = schema;
  mcp_register_tool(&tool);
}
