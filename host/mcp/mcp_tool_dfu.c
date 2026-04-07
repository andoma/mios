#include "../dfu.c"

#include "mcp_server.h"

static cJSON *
tool_flash_dfu(mcp_context_t *ctx, const cJSON *params, const char **errstr)
{
  const cJSON *elf_path = cJSON_GetObjectItem(params, "elf_path");
  if(!cJSON_IsString(elf_path)) {
    *errstr = "Missing required parameter: elf_path";
    return NULL;
  }

  const cJSON *force = cJSON_GetObjectItem(params, "force");
  int force_flash = cJSON_IsTrue(force);

  const char *err = dfu_flash_elf(ctx->usb, elf_path->valuestring, force_flash);
  if(err) {
    *errstr = err;
    return NULL;
  }

  return mcp_text_result("DFU flash successful");
}

static cJSON *schema;

void
mcp_tool_dfu_init(mcp_context_t *ctx)
{
  schema = cJSON_Parse(
    "{"
    "  \"type\": \"object\","
    "  \"properties\": {"
    "    \"elf_path\": {"
    "      \"type\": \"string\","
    "      \"description\": \"Path to ELF firmware file to flash\""
    "    },"
    "    \"force\": {"
    "      \"type\": \"boolean\","
    "      \"description\": \"Force flash even if build ID matches\","
    "      \"default\": false"
    "    }"
    "  },"
    "  \"required\": [\"elf_path\"]"
    "}");

  static mcp_tool_t tool = {
    .name = "flash_dfu",
    .description = "Flash firmware to an STM32 device via USB DFU. "
      "Automatically detects devices in DFU bootloader mode or "
      "running devices with DFU Runtime interface (sends DFU_DETACH "
      "and waits for re-enumeration). Parses the ELF file, compares "
      "build IDs to avoid unnecessary flashing, erases required "
      "sectors, and writes the image.",
    .handler = tool_flash_dfu,
  };
  tool.input_schema = schema;
  mcp_register_tool(&tool);
}
