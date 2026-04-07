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

static cJSON *
tool_configure(mcp_context_t *ctx, const cJSON *params, const char **errstr)
{
  (void)errstr;

  const cJSON *v = cJSON_GetObjectItem(params, "vid");
  if(cJSON_IsNumber(v))
    ctx->usb_vid = (uint16_t)v->valuedouble;

  const cJSON *p = cJSON_GetObjectItem(params, "pid");
  if(cJSON_IsNumber(p))
    ctx->usb_pid = (uint16_t)p->valuedouble;

  return mcp_text_resultf("Configured: VID=0x%04x PID=0x%04x",
                          ctx->usb_vid, ctx->usb_pid);
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
void mcp_tool_dfu_init(mcp_context_t *ctx);
void mcp_tool_stlink_init(mcp_context_t *ctx);
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
    "    }"
    "  }"
    "}");

  static mcp_tool_t cfg_tool = {
    .name = "configure",
    .description = "Set the USB VID and PID used to find MIOS devices. "
      "Affects all subsequent tool calls (cli, read_memory, sigcapture, "
      "flash_dfu). Default VID is 0x6666, PID is 0 (match any).",
    .handler = tool_configure,
  };
  cfg_tool.input_schema = cfg_schema;
  mcp_register_tool(&cfg_tool);

  mcp_tool_dfu_init(&ctx);
  mcp_tool_stlink_init(&ctx);
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
