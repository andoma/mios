#include "mcp_server.h"

#include "flash/flash.h"

#include <string.h>

static cJSON *
tool_flash(mcp_context_t *ctx, const cJSON *params, const char **errstr)
{
  (void)ctx;

  flash_params_t p = {};

  const cJSON *elf_path = cJSON_GetObjectItem(params, "elf_path");
  if(cJSON_IsString(elf_path))
    p.elf_path = elf_path->valuestring;

  const cJSON *method = cJSON_GetObjectItem(params, "method");
  if(cJSON_IsString(method))
    p.method = method->valuestring;

  const cJSON *cmdline = cJSON_GetObjectItem(params, "cmdline");
  if(cJSON_IsString(cmdline))
    p.cmdline = cmdline->valuestring;

  const cJSON *serial = cJSON_GetObjectItem(params, "probe_serial");
  if(cJSON_IsString(serial))
    p.serial = serial->valuestring;

  const cJSON *host = cJSON_GetObjectItem(params, "openocd_host");
  if(cJSON_IsString(host))
    p.openocd_host = host->valuestring;

  const cJSON *port = cJSON_GetObjectItem(params, "openocd_port");
  if(cJSON_IsNumber(port))
    p.openocd_port = (int)port->valuedouble;

  if(cJSON_IsTrue(cJSON_GetObjectItem(params, "force")))
    p.flags |= FLASH_FORCE;
  if(cJSON_IsTrue(cJSON_GetObjectItem(params, "no_verify")))
    p.flags |= FLASH_NO_VERIFY;
  if(cJSON_IsTrue(cJSON_GetObjectItem(params, "no_run")))
    p.flags |= FLASH_NO_RUN;
  if(cJSON_IsTrue(cJSON_GetObjectItem(params, "recover")))
    p.flags |= FLASH_RECOVER;
  if(cJSON_IsTrue(cJSON_GetObjectItem(params, "reset_only")))
    p.flags |= FLASH_RESET_ONLY;

  if(p.elf_path == NULL && !(p.flags & FLASH_RESET_ONLY)) {
    *errstr = "Missing required parameter: elf_path";
    return NULL;
  }

  flash_log_t log;
  flash_log_init(&log, NULL);
  const int r = flash_run(&p, &log);

  cJSON *result = mcp_text_resultf("%s%s", log.buf,
                                   r ? "\nFLASH FAILED" : "");
  flash_log_free(&log);
  return result;
}

static cJSON *schema;

void
mcp_tool_flash_init(mcp_context_t *ctx)
{
  (void)ctx;

  schema = cJSON_Parse(
    "{"
    "  \"type\": \"object\","
    "  \"properties\": {"
    "    \"elf_path\": {"
    "      \"type\": \"string\","
    "      \"description\": \"Path to ELF firmware file to flash\""
    "    },"
    "    \"method\": {"
    "      \"type\": \"string\","
    "      \"enum\": [\"auto\", \"jlink\", \"dfu\", \"openocd\"],"
    "      \"description\": \"jlink: SEGGER J-Link probe over SWD "
    "(nRF54L). dfu: STM32 USB DFU bootloader. openocd: running OpenOCD "
    "instance (ST-Link). auto: J-Link if a probe is connected, else "
    "DFU.\","
    "      \"default\": \"auto\""
    "    },"
    "    \"cmdline\": {"
    "      \"type\": \"string\","
    "      \"description\": \"Boot cmdline deposited in RAM (dfu)\""
    "    },"
    "    \"probe_serial\": {"
    "      \"type\": \"string\","
    "      \"description\": \"J-Link probe USB serial number (when "
    "multiple probes are connected)\""
    "    },"
    "    \"openocd_host\": { \"type\": \"string\" },"
    "    \"openocd_port\": { \"type\": \"integer\" },"
    "    \"force\": {"
    "      \"type\": \"boolean\","
    "      \"description\": \"Flash even if build ID matches (dfu)\""
    "    },"
    "    \"no_verify\": { \"type\": \"boolean\" },"
    "    \"no_run\": {"
    "      \"type\": \"boolean\","
    "      \"description\": \"Leave target halted after programming\""
    "    },"
    "    \"recover\": {"
    "      \"type\": \"boolean\","
    "      \"description\": \"Erase-all/unlock a protected chip first "
    "(jlink; wipes all data)\""
    "    },"
    "    \"reset_only\": {"
    "      \"type\": \"boolean\","
    "      \"description\": \"Don't program anything, just reset the "
    "target\""
    "    }"
    "  }"
    "}");

  static mcp_tool_t tool = {
    .name = "flash",
    .description = "Load firmware onto a target device. Backends: "
      "SEGGER J-Link (SWD, nRF54L targets, no J-Link software needed), "
      "STM32 USB DFU bootloader, or a running OpenOCD instance "
      "(ST-Link). Default method 'auto' picks J-Link if a probe is "
      "connected, else DFU.",
    .handler = tool_flash,
  };
  tool.input_schema = schema;
  mcp_register_tool(&tool);
}
