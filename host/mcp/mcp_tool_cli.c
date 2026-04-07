#include "mcp_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>

// Protocol message types (must match usb_mcp.c)
#define MCP_CLI_EXECUTE     0x01
#define MCP_CLI_RESPONSE    0x02
#define MCP_CLI_COMPLETE    0x03
#define MCP_MEM_READ        0x04
#define MCP_MEM_READ_RESP   0x05

#define USB_CLASS_VENDOR    0xff

// Find the MCP vendor interface on a MIOS device.
// Returns 0 on success, -1 if not found.
static int
find_mcp_interface(libusb_context *usb, uint16_t vid, uint16_t pid,
                   uint8_t subclass,
                   libusb_device_handle **handle_out,
                   uint8_t *iface_out,
                   uint8_t *ep_out_out,
                   uint8_t *ep_in_out)
{
  libusb_device **devlist;
  ssize_t cnt = libusb_get_device_list(usb, &devlist);

  for(ssize_t i = 0; i < cnt; i++) {
    struct libusb_device_descriptor desc;
    if(libusb_get_device_descriptor(devlist[i], &desc) != 0)
      continue;
    if(desc.idVendor != vid)
      continue;
    if(pid && desc.idProduct != pid)
      continue;

    struct libusb_config_descriptor *cfg;
    if(libusb_get_active_config_descriptor(devlist[i], &cfg) != 0)
      continue;

    int found = 0;
    for(int j = 0; j < cfg->bNumInterfaces && !found; j++) {
      const struct libusb_interface *iface = &cfg->interface[j];
      for(int a = 0; a < iface->num_altsetting && !found; a++) {
        const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
        if(alt->bInterfaceClass != USB_CLASS_VENDOR)
          continue;
        if(alt->bInterfaceSubClass != subclass)
          continue;
        if(alt->bNumEndpoints != 2)
          continue;

        uint8_t ep_out = 0, ep_in = 0;
        for(int e = 0; e < alt->bNumEndpoints; e++) {
          uint8_t addr = alt->endpoint[e].bEndpointAddress;
          if(addr & 0x80)
            ep_in = addr;
          else
            ep_out = addr;
        }

        if(ep_out && ep_in) {
          libusb_device_handle *h;
          if(libusb_open(devlist[i], &h) == 0) {
            *handle_out = h;
            *iface_out = alt->bInterfaceNumber;
            *ep_out_out = ep_out;
            *ep_in_out = ep_in;
            found = 1;
          }
        }
      }
    }
    libusb_free_config_descriptor(cfg);

    if(found) {
      libusb_free_device_list(devlist, 1);
      return 0;
    }
  }

  libusb_free_device_list(devlist, 1);
  return -1;
}


static cJSON *
tool_cli(mcp_context_t *ctx, const cJSON *params, const char **errstr)
{
  const cJSON *command = cJSON_GetObjectItem(params, "command");
  if(!cJSON_IsString(command)) {
    *errstr = "Missing required parameter: command";
    return NULL;
  }

  const cJSON *subclass_param = cJSON_GetObjectItem(params, "usb_subclass");
  uint8_t subclass = cJSON_IsNumber(subclass_param)
    ? (uint8_t)subclass_param->valuedouble : 1;

  uint16_t vid = ctx->usb_vid;
  uint16_t pid = ctx->usb_pid;

  libusb_device_handle *h;
  uint8_t iface_num, ep_out, ep_in;

  if(find_mcp_interface(ctx->usb, vid, pid, subclass,
                        &h, &iface_num, &ep_out, &ep_in)) {
    *errstr = "No MIOS device with MCP interface found";
    return NULL;
  }

  libusb_detach_kernel_driver(h, iface_num);
  if(libusb_claim_interface(h, iface_num)) {
    libusb_close(h);
    *errstr = "Failed to claim MCP USB interface";
    return NULL;
  }

  // Build and send CLI execute packet
  const char *cmd = command->valuestring;
  size_t cmdlen = strlen(cmd);
  if(cmdlen > 63) cmdlen = 63;

  uint8_t pkt[64];
  pkt[0] = MCP_CLI_EXECUTE;
  memcpy(pkt + 1, cmd, cmdlen);

  int transferred;
  int r = libusb_bulk_transfer(h, ep_out, pkt, 1 + cmdlen,
                               &transferred, 5000);
  if(r != 0) {
    libusb_release_interface(h, iface_num);
    libusb_close(h);
    *errstr = "Failed to send command";
    return NULL;
  }

  // Read responses
  size_t out_cap = 4096;
  size_t out_len = 0;
  char *output = malloc(out_cap);
  int32_t error_code = 0;
  int done = 0;

  const cJSON *timeout_param = cJSON_GetObjectItem(params, "timeout_ms");
  int timeout_ms = cJSON_IsNumber(timeout_param)
    ? (int)timeout_param->valuedouble : 5000;

  while(!done) {
    uint8_t resp[64];
    int resp_len;
    r = libusb_bulk_transfer(h, ep_in, resp, sizeof(resp),
                             &resp_len, timeout_ms);
    if(r != 0)
      break;
    if(resp_len < 1)
      continue;

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

  libusb_release_interface(h, iface_num);
  libusb_close(h);

  output[out_len] = '\0';

  cJSON *result;
  if(error_code && out_len == 0) {
    free(output);
    *errstr = "Command failed";
    return NULL;
  }

  result = mcp_text_result(output);
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
  if(length > 32) length = 32;

  const cJSON *subclass_param = cJSON_GetObjectItem(params, "usb_subclass");
  uint8_t subclass = cJSON_IsNumber(subclass_param)
    ? (uint8_t)subclass_param->valuedouble : 1;

  uint16_t vid = ctx->usb_vid;
  uint16_t pid = ctx->usb_pid;

  libusb_device_handle *h;
  uint8_t iface_num, ep_out, ep_in;

  if(find_mcp_interface(ctx->usb, vid, pid, subclass,
                        &h, &iface_num, &ep_out, &ep_in)) {
    *errstr = "No MIOS device with MCP interface found";
    return NULL;
  }

  libusb_detach_kernel_driver(h, iface_num);
  if(libusb_claim_interface(h, iface_num)) {
    libusb_close(h);
    *errstr = "Failed to claim MCP USB interface";
    return NULL;
  }

  // Send read memory request
  uint8_t pkt[9];
  pkt[0] = MCP_MEM_READ;
  memcpy(pkt + 1, &addr, 4);
  memcpy(pkt + 5, &length, 4);

  int transferred;
  int r = libusb_bulk_transfer(h, ep_out, pkt, sizeof(pkt),
                               &transferred, 5000);
  if(r != 0) {
    libusb_release_interface(h, iface_num);
    libusb_close(h);
    *errstr = "Failed to send read memory request";
    return NULL;
  }

  // Read response
  uint8_t resp[64];
  int resp_len;
  r = libusb_bulk_transfer(h, ep_in, resp, sizeof(resp), &resp_len, 5000);

  libusb_release_interface(h, iface_num);
  libusb_close(h);

  if(r != 0 || resp_len < 1 || resp[0] != MCP_MEM_READ_RESP) {
    *errstr = "Failed to read memory response";
    return NULL;
  }

  int data_len = resp_len - 1;

  // Format as hex dump
  char *hex = malloc(data_len * 3 + 64);
  int pos = sprintf(hex, "0x%08x:", addr);
  for(int i = 0; i < data_len; i++)
    pos += sprintf(hex + pos, " %02x", resp[1 + i]);
  hex[pos] = '\0';

  cJSON *result = mcp_text_result(hex);
  free(hex);
  return result;
}


static cJSON *cli_schema;
static cJSON *mem_schema;

void
mcp_tool_cli_init(mcp_context_t *ctx)
{
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
    "      \"description\": \"USB interface subclass for MCP interface\","
    "      \"default\": 1"
    "    }"
    "  },"
    "  \"required\": [\"command\"]"
    "}");


  static mcp_tool_t cli_tool = {
    .name = "cli",
    .description = "Send a CLI command to a connected MIOS device via "
      "the USB MCP interface. Returns the command output text and "
      "error code.",
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
    "      \"description\": \"Number of bytes to read (max 32)\","
    "      \"default\": 32"
    "    },"
    "    \"usb_subclass\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"USB interface subclass for MCP interface\","
    "      \"default\": 1"
    "    }"
    "  },"
    "  \"required\": [\"address\"]"
    "}");


  static mcp_tool_t mem_tool = {
    .name = "read_memory",
    .description = "Read raw memory from a connected MIOS device. "
      "Returns a hex dump of the specified address range. "
      "Max 32 bytes per read.",
    .handler = tool_read_memory,
  };
  mem_tool.input_schema = mem_schema;
  mcp_register_tool(&mem_tool);
}
