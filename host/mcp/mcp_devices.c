#include "mcp_devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static int
get_u32(const cJSON *obj, const char *key, uint32_t *out)
{
  const cJSON *v = cJSON_GetObjectItem(obj, key);
  if(cJSON_IsNumber(v)) {
    *out = (uint32_t)v->valuedouble;
    return 0;
  }
  if(cJSON_IsString(v) && v->valuestring[0]) {
    *out = (uint32_t)strtoul(v->valuestring, NULL, 0);
    return 0;
  }
  return -1;
}

int
mcp_devices_load(const char *path, mcp_device_t **out, const char **errstr)
{
  FILE *fp = fopen(path, "rb");
  if(fp == NULL) {
    *errstr = "Failed to open device list file";
    return -1;
  }
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char *buf = malloc(size + 1);
  size_t rd = fread(buf, 1, size, fp);
  fclose(fp);
  buf[rd] = '\0';

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if(root == NULL || !cJSON_IsArray(root)) {
    cJSON_Delete(root);
    *errstr = "Device list is not a JSON array";
    return -1;
  }

  int n = cJSON_GetArraySize(root);
  mcp_device_t *devices = calloc(n, sizeof(*devices));
  int count = 0;

  for(int i = 0; i < n; i++) {
    const cJSON *item = cJSON_GetArrayItem(root, i);
    const cJSON *name = cJSON_GetObjectItem(item, "name");
    const cJSON *transport = cJSON_GetObjectItem(item, "transport");
    if(!cJSON_IsString(name) || !cJSON_IsString(transport))
      continue; // malformed entry, skip rather than fail the whole file

    uint32_t tx, rx;
    if(get_u32(item, "vllp_tx", &tx) || get_u32(item, "vllp_rx", &rx))
      continue;

    devices[count].name = strdup(name->valuestring);
    devices[count].transport = strdup(transport->valuestring);
    devices[count].vllp_tx = tx;
    devices[count].vllp_rx = rx;
    count++;
  }

  cJSON_Delete(root);
  *out = devices;
  return count;
}

void
mcp_devices_free(mcp_device_t *devices, int count)
{
  for(int i = 0; i < count; i++) {
    free(devices[i].name);
    free(devices[i].transport);
  }
  free(devices);
}

const mcp_device_t *
mcp_devices_find(const mcp_device_t *devices, int count, const char *name)
{
  for(int i = 0; i < count; i++) {
    if(!strcmp(devices[i].name, name))
      return &devices[i];
  }
  return NULL;
}
