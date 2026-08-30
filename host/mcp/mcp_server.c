#include "mcp_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#define MAX_TOOLS 16

static mcp_tool_t *tools[MAX_TOOLS];
static int num_tools;

void
mcp_register_tool(mcp_tool_t *tool)
{
  if(num_tools < MAX_TOOLS)
    tools[num_tools++] = tool;
}

cJSON *
mcp_text_result(const char *text)
{
  cJSON *content = cJSON_CreateArray();
  cJSON *item = cJSON_CreateObject();
  cJSON_AddStringToObject(item, "type", "text");
  cJSON_AddStringToObject(item, "text", text);
  cJSON_AddItemToArray(content, item);
  return content;
}

cJSON *
mcp_text_resultf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char *buf = NULL;
  if(vasprintf(&buf, fmt, ap) < 0)
    buf = NULL;
  va_end(ap);
  cJSON *r = mcp_text_result(buf ? buf : "out of memory");
  free(buf);
  return r;
}

// --- configure tool ---

static void
set_vllp_target(mcp_context_t *ctx, const char *transport,
                uint32_t tx, uint32_t rx)
{
  free(ctx->vllp_transport);
  ctx->vllp_transport = strdup(transport);
  ctx->vllp_tx = tx;
  ctx->vllp_rx = rx;
}

static cJSON *
tool_configure(mcp_context_t *ctx, const cJSON *params, const char **errstr)
{
  const cJSON *v = cJSON_GetObjectItem(params, "vid");
  if(cJSON_IsNumber(v))
    ctx->usb_vid = (uint16_t)v->valuedouble;

  const cJSON *p = cJSON_GetObjectItem(params, "pid");
  if(cJSON_IsNumber(p))
    ctx->usb_pid = (uint16_t)p->valuedouble;

  const cJSON *s = cJSON_GetObjectItem(params, "serial");
  if(cJSON_IsString(s)) {
    free(ctx->serial);
    ctx->serial = s->valuestring[0] ? strdup(s->valuestring) : NULL;
  }

  const cJSON *mtu = cJSON_GetObjectItem(params, "vllp_mtu");
  if(cJSON_IsNumber(mtu))
    ctx->vllp_mtu = (int)mtu->valuedouble;

  const cJSON *to = cJSON_GetObjectItem(params, "vllp_timeout_s");
  if(cJSON_IsNumber(to))
    ctx->vllp_timeout_s = (int)to->valuedouble;

  // Named device: looked up from $MIOS_MCP_DEVICES.
  const cJSON *device = cJSON_GetObjectItem(params, "device");
  if(cJSON_IsString(device)) {
    if(device->valuestring[0] == '\0') {
      free(ctx->vllp_transport);
      ctx->vllp_transport = NULL;
    } else {
      const mcp_device_t *d = mcp_devices_find(ctx->devices, ctx->num_devices,
                                               device->valuestring);
      if(d == NULL) {
        *errstr = "Unknown device name (see the scan tool for what's "
          "configured in $MIOS_MCP_DEVICES)";
        return NULL;
      }
      set_vllp_target(ctx, d->transport, d->vllp_tx, d->vllp_rx);
    }
  }

  // Raw VLLP override, for a target not in the device list. transport
  // alone with no vllp_tx/vllp_rx (or vice versa) is a usage error --
  // all three are needed to make sense together.
  const cJSON *transport = cJSON_GetObjectItem(params, "transport");
  const cJSON *tx = cJSON_GetObjectItem(params, "vllp_tx");
  const cJSON *rx = cJSON_GetObjectItem(params, "vllp_rx");
  if(transport || tx || rx) {
    if(!cJSON_IsString(transport) || !cJSON_IsNumber(tx) || !cJSON_IsNumber(rx)) {
      *errstr = "transport, vllp_tx and vllp_rx must all be given together";
      return NULL;
    }
    set_vllp_target(ctx, transport->valuestring, (uint32_t)tx->valuedouble,
                    (uint32_t)rx->valuedouble);
  }

  if(ctx->vllp_transport)
    return mcp_text_resultf("Configured: VLLP transport=%s tx=0x%x rx=0x%x",
                            ctx->vllp_transport, ctx->vllp_tx, ctx->vllp_rx);

  return mcp_text_resultf("Configured: VID=0x%04x PID=0x%04x transport=%s%s",
                          ctx->usb_vid, ctx->usb_pid,
                          ctx->serial ? "serial " : "usb",
                          ctx->serial ? ctx->serial : "");
}

// --- JSON-RPC helpers ---

static void
send_json(cJSON *json)
{
  char *str = cJSON_PrintUnformatted(json);
  if(str) {
    fprintf(stdout, "%s\n", str);
    fflush(stdout);
    free(str);
  }
  cJSON_Delete(json);
}

static cJSON *
make_response(const cJSON *id)
{
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  if(id)
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
  return resp;
}

static void
send_error(const cJSON *id, int code, const char *message)
{
  cJSON *resp = make_response(id);
  cJSON *err = cJSON_CreateObject();
  cJSON_AddNumberToObject(err, "code", code);
  cJSON_AddStringToObject(err, "message", message);
  cJSON_AddItemToObject(resp, "error", err);
  send_json(resp);
}

// --- MCP method handlers ---

static void
handle_initialize(const cJSON *id)
{
  cJSON *resp = make_response(id);
  cJSON *result = cJSON_CreateObject();

  cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");

  cJSON *caps = cJSON_CreateObject();
  cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
  cJSON_AddItemToObject(result, "capabilities", caps);

  cJSON *info = cJSON_CreateObject();
  cJSON_AddStringToObject(info, "name", "mios");
  cJSON_AddStringToObject(info, "version", "0.1.0");
  cJSON_AddItemToObject(result, "serverInfo", info);

  cJSON_AddItemToObject(resp, "result", result);
  send_json(resp);
}

static void
handle_tools_list(const cJSON *id)
{
  cJSON *resp = make_response(id);
  cJSON *result = cJSON_CreateObject();
  cJSON *tool_array = cJSON_CreateArray();

  for(int i = 0; i < num_tools; i++) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", tools[i]->name);
    cJSON_AddStringToObject(t, "description", tools[i]->description);
    if(tools[i]->input_schema)
      cJSON_AddItemToObject(t, "inputSchema",
                            cJSON_Duplicate(tools[i]->input_schema, 1));
    cJSON_AddItemToArray(tool_array, t);
  }

  cJSON_AddItemToObject(result, "tools", tool_array);
  cJSON_AddItemToObject(resp, "result", result);
  send_json(resp);
}

static void
handle_tools_call(mcp_context_t *ctx, const cJSON *id, const cJSON *params)
{
  const cJSON *name = cJSON_GetObjectItem(params, "name");
  if(!cJSON_IsString(name)) {
    send_error(id, -32602, "Missing tool name");
    return;
  }

  const cJSON *arguments = cJSON_GetObjectItem(params, "arguments");

  mcp_tool_t *tool = NULL;
  for(int i = 0; i < num_tools; i++) {
    if(!strcmp(tools[i]->name, name->valuestring)) {
      tool = tools[i];
      break;
    }
  }

  if(!tool) {
    send_error(id, -32602, "Unknown tool");
    return;
  }

  const char *errstr = NULL;
  cJSON *content = tool->handler(ctx, arguments, &errstr);

  cJSON *resp = make_response(id);
  cJSON *result = cJSON_CreateObject();

  if(content) {
    cJSON_AddItemToObject(result, "content", content);
  } else {
    cJSON_AddItemToObject(result, "content",
                          mcp_text_result(errstr ? errstr : "Unknown error"));
    cJSON_AddBoolToObject(result, "isError", 1);
  }

  cJSON_AddItemToObject(resp, "result", result);
  send_json(resp);
}

// --- Main loop ---

static void
dispatch(mcp_context_t *ctx, cJSON *msg)
{
  const cJSON *method = cJSON_GetObjectItem(msg, "method");
  const cJSON *id = cJSON_GetObjectItem(msg, "id");
  const cJSON *params = cJSON_GetObjectItem(msg, "params");

  if(!cJSON_IsString(method)) {
    if(id)
      send_error(id, -32600, "Missing method");
    return;
  }

  const char *m = method->valuestring;

  if(!strcmp(m, "initialize")) {
    handle_initialize(id);
  } else if(!strcmp(m, "notifications/initialized")) {
    // No-op
  } else if(!strcmp(m, "tools/list")) {
    handle_tools_list(id);
  } else if(!strcmp(m, "tools/call")) {
    handle_tools_call(ctx, id, params);
  } else {
    if(id)
      send_error(id, -32601, "Method not found");
  }
}

// Tool init functions (defined in each tool file)
void mcp_tool_flash_init(mcp_context_t *ctx);
void mcp_tool_cli_init(mcp_context_t *ctx);
void mcp_tool_sigcapture_init(mcp_context_t *ctx);

int
main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  mcp_context_t ctx = {
    .usb_vid = 0x6666,
    .usb_pid = 0,
  };

  // Default transport can be set from the environment or argv so the server
  // can be pointed at a serial port without an explicit configure() call.
  const char *env_serial = getenv("MIOS_MCP_SERIAL");
  for(int i = 1; i < argc; i++) {
    if(!strcmp(argv[i], "--serial") && i + 1 < argc)
      env_serial = argv[++i];
  }
  if(env_serial && env_serial[0])
    ctx.serial = strdup(env_serial);

  // Named VLLP devices, entirely project-defined -- mios itself never
  // hardcodes a board name or transport address.
  const char *devices_path = getenv("MIOS_MCP_DEVICES");
  if(devices_path && devices_path[0]) {
    const char *errstr = NULL;
    int n = mcp_devices_load(devices_path, &ctx.devices, &errstr);
    if(n < 0) {
      fprintf(stderr, "mios-mcp: %s (%s)\n", errstr, devices_path);
    } else {
      ctx.num_devices = n;
    }
  }

  if(libusb_init(&ctx.usb)) {
    fprintf(stderr, "libusb_init failed\n");
    return 1;
  }

  // Register configure tool
  static cJSON *cfg_schema;
  cfg_schema = cJSON_Parse(
    "{"
    "  \"type\": \"object\","
    "  \"properties\": {"
    "    \"vid\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"USB Vendor ID in hex (e.g. 0x6666)\""
    "    },"
    "    \"pid\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"USB Product ID in hex (0 = match any)\""
    "    },"
    "    \"serial\": {"
    "      \"type\": \"string\","
    "      \"description\": \"Serial device path (e.g. /dev/ttyACM4) for "
    "HDLC-framed MCP over UART. Empty string reverts to USB.\""
    "    },"
    "    \"device\": {"
    "      \"type\": \"string\","
    "      \"description\": \"Name of a device from $MIOS_MCP_DEVICES to "
    "reach over VLLP (see the scan tool for what's configured). Empty "
    "string reverts to USB/serial.\""
    "    },"
    "    \"transport\": {"
    "      \"type\": \"string\","
    "      \"description\": \"Raw VLLP target not in the device list: a "
    "dsig transport address, e.g. 'file:///tmp/fcmon.sock', 'usb', 'udp', "
    "or 'cansock'. Must be given together with vllp_tx and vllp_rx.\""
    "    },"
    "    \"vllp_tx\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"dsig signal id for host->device (raw VLLP "
    "target only)\""
    "    },"
    "    \"vllp_rx\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"dsig signal id for device->host (raw VLLP "
    "target only)\""
    "    },"
    "    \"vllp_mtu\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"VLLP link MTU (default 64, for FDCAN)\""
    "    },"
    "    \"vllp_timeout_s\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"VLLP retransmit timeout in seconds (default 3)\""
    "    }"
    "  }"
    "}");

  static mcp_tool_t cfg_tool = {
    .name = "configure",
    .description = "Set how subsequent tool calls (cli, read_memory, "
      "sigcapture, flash_dfu) reach a MIOS device: directly over USB "
      "(vid/pid, default VID 0x6666 PID 0=any), over serial (serial), or "
      "over VLLP riding a dsig bus (device, or transport+vllp_tx+vllp_rx "
      "for a target not in the device list).",
    .handler = tool_configure,
  };
  cfg_tool.input_schema = cfg_schema;
  mcp_register_tool(&cfg_tool);

  mcp_tool_flash_init(&ctx);
  mcp_tool_cli_init(&ctx);
  mcp_tool_sigcapture_init(&ctx);

  char *line = NULL;
  size_t cap = 0;
  ssize_t len;

  while((len = getline(&line, &cap, stdin)) > 0) {
    if(len > 0 && line[len - 1] == '\n')
      line[--len] = '\0';
    if(len == 0)
      continue;

    cJSON *msg = cJSON_Parse(line);
    if(!msg) {
      send_error(NULL, -32700, "Parse error");
      continue;
    }

    dispatch(&ctx, msg);
    cJSON_Delete(msg);
  }

  free(line);
  libusb_exit(ctx.usb);
  return 0;
}
