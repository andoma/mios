#include "mcp_server.h"
#include "mcp_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Protocol message types (must match usb_mcp.c / mcp_uart.c)
#define MCP_CLI_EXECUTE     0x01
#define MCP_CLI_RESPONSE    0x02
#define MCP_CLI_COMPLETE    0x03
#define MCP_MEM_READ        0x04
#define MCP_MEM_READ_RESP   0x05

// Names for error_t codes (negative). Index = -code.
// Mirrors the enum in include/mios/error.h and errmsg[] in
// src/shell/cli.c; keep in sync when error codes are added.
static const char *const err_names[] = {
  "OK", "NOT_IMPLEMENTED", "TIMEOUT", "OPERATION_FAILED", "TX_FAULT",
  "RX_FAULT", "NOT_READY", "NO_BUFFER", "MTU_EXCEEDED", "INVALID_ID",
  "DMAXFER", "BUS_ERR", "ARBITRATION_LOST", "BAD_STATE", "INVALID_ADDRESS",
  "NO_DEVICE", "MISMATCH", "NOT_FOUND", "CHECKSUM_ERR", "MALFORMED",
  "INVALID_RPC_ID", "INVALID_RPC_ARGS", "NO_FLASH_SPACE", "INVALID_ARGS",
  "INVALID_LENGTH", "NOT_IDLE", "BAD_CONFIG", "FLASH_HW_ERR", "FLASH_TIMEOUT",
  "NO_MEMORY", "READ_PROT", "WRITE_PROT", "AGAIN", "NOT_CONNECTED",
  "BAD_PKT_SIZ", "EXISTS", "CORRUPT", "NOT_DIR", "IS_DIR", "NOT_EMPTY",
  "BADF", "TOOBIG", "INVALID_PARAMETER", "NOTATTR", "TOOLONG", "IO",
  "FS", "DMAFIFO", "INTERRUPTED", "QUEUE_FULL", "NO_ROUTE",
};

static const char *
cli_error_name(int32_t code)
{
  int idx = -code;
  if(idx < 0 || idx >= (int)(sizeof(err_names) / sizeof(err_names[0])))
    return "UNKNOWN";
  return err_names[idx];
}


static uint8_t
mcp_subclass(const cJSON *params)
{
  const cJSON *p = cJSON_GetObjectItem(params, "usb_subclass");
  return cJSON_IsNumber(p) ? (uint8_t)p->valuedouble : 1;
}


static cJSON *
tool_cli(mcp_context_t *ctx, const cJSON *params, const char **errstr)
{
  const cJSON *command = cJSON_GetObjectItem(params, "command");
  if(!cJSON_IsString(command)) {
    *errstr = "Missing required parameter: command";
    return NULL;
  }

  const cJSON *timeout_param = cJSON_GetObjectItem(params, "timeout_ms");
  int timeout_ms = cJSON_IsNumber(timeout_param)
    ? (int)timeout_param->valuedouble : 5000;

  mcp_xport_t *x = mcp_xport_open(ctx, mcp_subclass(params), errstr);
  if(!x)
    return NULL;

  const char *cmd = command->valuestring;
  size_t cmdlen = strlen(cmd);
  size_t maxp = mcp_xport_max_payload(x);
  if(cmdlen > maxp)
    cmdlen = maxp;

  uint8_t *pkt = malloc(1 + cmdlen);
  pkt[0] = MCP_CLI_EXECUTE;
  memcpy(pkt + 1, cmd, cmdlen);
  int sr = mcp_xport_send(x, pkt, 1 + cmdlen);
  free(pkt);

  if(sr) {
    mcp_xport_close(x);
    *errstr = "Failed to send command";
    return NULL;
  }

  size_t out_cap = 4096;
  size_t out_len = 0;
  char *output = malloc(out_cap);
  int32_t error_code = 0;
  int done = 0;

  while(!done) {
    uint8_t resp[512];
    int resp_len = mcp_xport_recv(x, resp, sizeof(resp), timeout_ms);
    if(resp_len <= 0)
      break;

    switch(resp[0]) {
    case MCP_CLI_RESPONSE: {
      int data_len = resp_len - 1;
      if(out_len + data_len >= out_cap) {
        out_cap = (out_len + data_len) * 2;
        output = realloc(output, out_cap);
      }
      memcpy(output + out_len, resp + 1, data_len);
      out_len += data_len;
      break;
    }
    case MCP_CLI_COMPLETE:
      if(resp_len >= 5)
        memcpy(&error_code, resp + 1, 4);
      done = 1;
      break;
    default:
      break;
    }
  }

  mcp_xport_close(x);
  output[out_len] = '\0';

  if(error_code) {
    while(out_len &&
          (output[out_len - 1] == '\n' || output[out_len - 1] == '\r'))
      output[--out_len] = '\0';
    static char buf[512];
    if(out_len)
      snprintf(buf, sizeof(buf), "%s: %s (error %d)",
               output, cli_error_name(error_code), error_code);
    else
      snprintf(buf, sizeof(buf), "%s (error %d)",
               cli_error_name(error_code), error_code);
    free(output);
    *errstr = buf;
    return NULL;
  }

  cJSON *result = mcp_text_result(output);
  free(output);
  return result;
}


static cJSON *
tool_read_memory(mcp_context_t *ctx, const cJSON *params, const char **errstr)
{
  const cJSON *addr_param = cJSON_GetObjectItem(params, "address");
  const cJSON *len_param = cJSON_GetObjectItem(params, "length");

  if(!cJSON_IsNumber(addr_param)) {
    *errstr = "Missing required parameter: address";
    return NULL;
  }

  uint32_t addr = (uint32_t)addr_param->valuedouble;
  uint32_t length = cJSON_IsNumber(len_param)
    ? (uint32_t)len_param->valuedouble : 32;

  mcp_xport_t *x = mcp_xport_open(ctx, mcp_subclass(params), errstr);
  if(!x)
    return NULL;

  size_t maxp = mcp_xport_max_payload(x);
  if(length > maxp)
    length = maxp;

  uint8_t pkt[9];
  pkt[0] = MCP_MEM_READ;
  memcpy(pkt + 1, &addr, 4);
  memcpy(pkt + 5, &length, 4);

  if(mcp_xport_send(x, pkt, sizeof(pkt))) {
    mcp_xport_close(x);
    *errstr = "Failed to send read memory request";
    return NULL;
  }

  // Skip unrelated frames (e.g. the periodic hello beacon) until the reply.
  uint8_t resp[512];
  int resp_len;
  do {
    resp_len = mcp_xport_recv(x, resp, sizeof(resp), 5000);
  } while(resp_len >= 1 && resp[0] != MCP_MEM_READ_RESP);
  mcp_xport_close(x);

  if(resp_len < 1 || resp[0] != MCP_MEM_READ_RESP) {
    *errstr = "Failed to read memory response";
    return NULL;
  }

  int data_len = resp_len - 1;

  char *hex = malloc(data_len * 3 + 64);
  int pos = sprintf(hex, "0x%08x:", addr);
  for(int i = 0; i < data_len; i++)
    pos += sprintf(hex + pos, " %02x", resp[1 + i]);
  hex[pos] = '\0';

  cJSON *result = mcp_text_result(hex);
  free(hex);
  return result;
}


// Briefly opens a VLLP transport to `d` and exercises the exact path
// tool_cli() would use (an empty CLI command), to check whether anything
// is actually alive on the far end -- a VLLP channel can be created
// without a live peer (SYN retries happen in the background), so
// "opened successfully" alone says nothing about reachability.
// ctx->vllp_* is saved/restored around the probe, leaving the caller's
// own configuration untouched.
static int
device_probe(mcp_context_t *ctx, const mcp_device_t *d)
{
  char *saved_transport = ctx->vllp_transport;
  uint32_t saved_tx = ctx->vllp_tx, saved_rx = ctx->vllp_rx;

  ctx->vllp_transport = d->transport;
  ctx->vllp_tx = d->vllp_tx;
  ctx->vllp_rx = d->vllp_rx;

  const char *errstr;
  mcp_xport_t *x = mcp_xport_open(ctx, 0, &errstr);

  ctx->vllp_transport = saved_transport;
  ctx->vllp_tx = saved_tx;
  ctx->vllp_rx = saved_rx;

  if(x == NULL)
    return 0; // transport itself unreachable (e.g. fcmon not running)

  uint8_t pkt = MCP_CLI_EXECUTE; // empty command, just eliciting a reply
  int alive = 0;
  if(mcp_xport_send(x, &pkt, 1) == 0) {
    uint8_t resp[64];
    // Generous: covers the whole VLLP handshake (SYN/ACK, channel
    // open-request/response) plus the actual exchange, not just one
    // hop's worth of latency -- 800ms measured too tight in practice.
    int r = mcp_xport_recv(x, resp, sizeof(resp), 2500);
    alive = r >= 1 && (resp[0] == MCP_CLI_COMPLETE || resp[0] == MCP_CLI_RESPONSE);
  }
  mcp_xport_close(x);
  return alive;
}

static cJSON *
tool_scan(mcp_context_t *ctx, const cJSON *params, const char **errstr)
{
  (void)params;
  (void)errstr;

  size_t cap = 4096;
  char *out = malloc(cap);
  int pos = 0;

  char paths[16][256];
  int n = mcp_serial_scan(paths, 16);
  if(n == 0) {
    pos += snprintf(out + pos, cap - pos,
                    "No MCP serial ports found (no hello beacon seen on "
                    "any /dev/ttyACM*//dev/ttyUSB*).\n");
  } else {
    pos += snprintf(out + pos, cap - pos, "Found %d MCP serial port%s:\n",
                    n, n == 1 ? "" : "s");
    for(int i = 0; i < n; i++)
      pos += snprintf(out + pos, cap - pos, "  %s\n", paths[i]);
  }

  if(ctx->num_devices == 0) {
    pos += snprintf(out + pos, cap - pos,
                    "\nNo VLLP devices configured ($MIOS_MCP_DEVICES unset "
                    "or empty).\n");
  } else {
    pos += snprintf(out + pos, cap - pos,
                    "\n%d VLLP device%s configured (probing each, ~2.5s):\n",
                    ctx->num_devices, ctx->num_devices == 1 ? "" : "s");
    for(int i = 0; i < ctx->num_devices; i++) {
      const mcp_device_t *d = &ctx->devices[i];
      int alive = device_probe(ctx, d);
      pos += snprintf(out + pos, cap - pos,
                      "  %-20s %s tx=0x%x rx=0x%x  [%s]\n",
                      d->name, d->transport, d->vllp_tx, d->vllp_rx,
                      alive ? "alive" : "no response");
    }
    pos += snprintf(out + pos, cap - pos,
                    "\nUse configure(device: \"<name>\") to target one.\n");
  }

  cJSON *r = mcp_text_result(out);
  free(out);
  return r;
}


static cJSON *cli_schema;
static cJSON *mem_schema;
static cJSON *scan_schema;

void
mcp_tool_cli_init(mcp_context_t *ctx)
{
  (void)ctx;

  cli_schema = cJSON_Parse(
    "{"
    "  \"type\": \"object\","
    "  \"properties\": {"
    "    \"command\": {"
    "      \"type\": \"string\","
    "      \"description\": \"CLI command to send to the device\""
    "    },"
    "    \"timeout_ms\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"Response timeout in milliseconds\","
    "      \"default\": 5000"
    "    },"
    "    \"usb_subclass\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"USB interface subclass for MCP interface "
    "(ignored for serial transport)\","
    "      \"default\": 1"
    "    }"
    "  },"
    "  \"required\": [\"command\"]"
    "}");


  static mcp_tool_t cli_tool = {
    .name = "cli",
    .description = "Send a CLI command to a connected MIOS device via the "
      "MCP interface (USB, or serial/HDLC when configured). Returns the "
      "command output text and error code.",
    .handler = tool_cli,
  };
  cli_tool.input_schema = cli_schema;
  mcp_register_tool(&cli_tool);

  mem_schema = cJSON_Parse(
    "{"
    "  \"type\": \"object\","
    "  \"properties\": {"
    "    \"address\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"Memory address to read (e.g. 0x08000000)\""
    "    },"
    "    \"length\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"Number of bytes to read (USB: max 32, "
    "serial: max 255)\","
    "      \"default\": 32"
    "    },"
    "    \"usb_subclass\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"USB interface subclass for MCP interface "
    "(ignored for serial transport)\","
    "      \"default\": 1"
    "    }"
    "  },"
    "  \"required\": [\"address\"]"
    "}");


  static mcp_tool_t mem_tool = {
    .name = "read_memory",
    .description = "Read raw memory from a connected MIOS device. "
      "Returns a hex dump of the specified address range.",
    .handler = tool_read_memory,
  };
  mem_tool.input_schema = mem_schema;
  mcp_register_tool(&mem_tool);

  scan_schema = cJSON_Parse(
    "{ \"type\": \"object\", \"properties\": {} }");

  static mcp_tool_t scan_tool = {
    .name = "scan",
    .description = "Scan serial ports (/dev/ttyACM*, /dev/ttyUSB*) for MIOS "
      "devices emitting an MCP hello beacon, and probe every VLLP device "
      "configured in $MIOS_MCP_DEVICES for a live response. Use a result "
      "with configure(serial: ...)/configure(serial: \"auto\") or "
      "configure(device: \"<name>\").",
    .handler = tool_scan,
  };
  scan_tool.input_schema = scan_schema;
  mcp_register_tool(&scan_tool);
}
