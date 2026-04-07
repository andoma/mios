#include "mcp_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <libusb.h>

// Wire protocol structs (must match device side in sigcapture.c)
typedef struct {
  uint8_t pkt_type;   // 0xff = preamble
  uint8_t channels;
  uint16_t depth;
  uint32_t nominal_frequency;
  int16_t trig_offset;
} __attribute__((packed)) sc_preamble_t;

typedef struct {
  uint8_t pkt_type;   // channel index
  uint8_t unit;
  char name[14];
  float scale;
} __attribute__((packed)) sc_channel_desc_t;

static const char *unit_names[] = {
  "unused", "1", "V", "A", "\xc2\xb0""C", "digital"
};

#define SIGCAPTURE_MAX_STORED   16
#define SIGCAPTURE_MAX_CHANNELS 32
#define USB_CLASS_VENDOR        0xff

typedef struct {
  char name[15];
  char unit[8];
  float scale;
  int digital;
} sc_channel_t;

typedef struct {
  time_t timestamp;
  uint32_t id;
  uint32_t sample_rate;
  int16_t trig_offset;
  int num_channels;
  int depth;
  sc_channel_t channels[SIGCAPTURE_MAX_CHANNELS];
  float *data;  // num_channels * depth floats, pre-scaled
} sc_capture_t;

// In-progress capture being received from USB
typedef struct {
  int num_channels;
  int depth;
  uint32_t sample_rate;
  int16_t trig_offset;
  int columns_per_xfer;
  sc_channel_t channels[SIGCAPTURE_MAX_CHANNELS];
  float *data;
  int sample_offset;
} sc_recv_t;

typedef struct {
  pthread_mutex_t mutex;
  pthread_t thread;
  sc_capture_t captures[SIGCAPTURE_MAX_STORED];
  int num_captures;
  uint32_t next_id;

  libusb_context *usb;
  uint16_t vid;
  uint16_t pid;
  uint8_t subclass;
} sc_state_t;

static sc_state_t sc_state;

// --- Ring buffer management ---

static void
sc_store_capture(sc_state_t *st, sc_recv_t *recv)
{
  pthread_mutex_lock(&st->mutex);

  // If ring buffer full, free oldest
  if(st->num_captures == SIGCAPTURE_MAX_STORED) {
    free(st->captures[0].data);
    memmove(&st->captures[0], &st->captures[1],
            (SIGCAPTURE_MAX_STORED - 1) * sizeof(sc_capture_t));
    st->num_captures--;
  }

  sc_capture_t *cap = &st->captures[st->num_captures];
  cap->timestamp = time(NULL);
  cap->id = st->next_id++;
  cap->sample_rate = recv->sample_rate;
  cap->trig_offset = recv->trig_offset;
  cap->num_channels = recv->num_channels;
  cap->depth = recv->sample_offset;  // Actual samples received
  memcpy(cap->channels, recv->channels, sizeof(recv->channels));
  cap->data = recv->data;
  recv->data = NULL;  // Ownership transferred

  st->num_captures++;
  pthread_mutex_unlock(&st->mutex);
}

// --- Protocol parsing ---

static int
sc_handle_pkt(sc_recv_t *recv, const uint8_t *pkt, int len)
{
  if(len == (int)sizeof(sc_preamble_t)) {
    // Preamble
    const sc_preamble_t *p = (const sc_preamble_t *)pkt;
    recv->num_channels = p->channels;
    recv->depth = p->depth;
    recv->sample_rate = p->nominal_frequency;
    recv->trig_offset = p->trig_offset;
    recv->sample_offset = 0;
    if(recv->num_channels > 0)
      recv->columns_per_xfer = 32 / recv->num_channels;
    else
      recv->columns_per_xfer = 0;

    free(recv->data);
    recv->data = calloc(recv->depth * recv->num_channels, sizeof(float));
    return 0;

  } else if(len == (int)sizeof(sc_channel_desc_t)) {
    // Channel descriptor
    const sc_channel_desc_t *c = (const sc_channel_desc_t *)pkt;
    int ch = c->pkt_type;
    if(ch >= SIGCAPTURE_MAX_CHANNELS)
      return 0;

    memset(recv->channels[ch].name, 0, sizeof(recv->channels[ch].name));
    memcpy(recv->channels[ch].name, c->name, 14);
    recv->channels[ch].scale = c->scale;
    recv->channels[ch].digital = (c->unit == 5);  // SIGCAPTURE_UNIT_DIGITAL

    int unit_idx = c->unit;
    if(unit_idx < (int)(sizeof(unit_names) / sizeof(unit_names[0])))
      snprintf(recv->channels[ch].unit, sizeof(recv->channels[ch].unit),
               "%s", unit_names[unit_idx]);
    else
      snprintf(recv->channels[ch].unit, sizeof(recv->channels[ch].unit),
               "?");
    return 0;

  } else if(len == 1) {
    // Trailer (0xfe) — capture complete
    return 1;

  } else if(len == 64 && recv->data) {
    // Data packet
    const int16_t *src = (const int16_t *)pkt;
    for(int i = 0; i < recv->columns_per_xfer; i++) {
      if(recv->sample_offset >= recv->depth)
        break;
      for(int j = 0; j < recv->num_channels; j++) {
        int idx = recv->sample_offset * recv->num_channels + j;
        recv->data[idx] = *src++ * recv->channels[j].scale;
      }
      recv->sample_offset++;
    }
    return 0;
  }
  return 0;
}

// --- USB device discovery ---

static int
sc_find_device(sc_state_t *st,
               libusb_device_handle **handle_out,
               uint8_t *iface_out, uint8_t *ep_out)
{
  libusb_device **devlist;
  ssize_t cnt = libusb_get_device_list(st->usb, &devlist);

  for(ssize_t i = 0; i < cnt; i++) {
    struct libusb_device_descriptor desc;
    if(libusb_get_device_descriptor(devlist[i], &desc) != 0)
      continue;
    if(desc.idVendor != st->vid)
      continue;
    if(st->pid && desc.idProduct != st->pid)
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
        if(alt->bInterfaceSubClass != st->subclass)
          continue;
        if(alt->bNumEndpoints != 1)
          continue;
        if(!(alt->endpoint[0].bEndpointAddress & 0x80))
          continue;

        libusb_device_handle *h;
        if(libusb_open(devlist[i], &h) == 0) {
          *handle_out = h;
          *iface_out = alt->bInterfaceNumber;
          *ep_out = alt->endpoint[0].bEndpointAddress;
          found = 1;
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

// --- Background thread ---

static void *
sc_bg_thread(void *arg)
{
  sc_state_t *st = arg;
  sc_recv_t recv = {};

  while(1) {
    libusb_device_handle *h;
    uint8_t iface_num, ep_addr;

    if(sc_find_device(st, &h, &iface_num, &ep_addr)) {
      sleep(1);
      continue;
    }

    libusb_detach_kernel_driver(h, iface_num);
    if(libusb_claim_interface(h, iface_num)) {
      libusb_close(h);
      sleep(1);
      continue;
    }

    uint8_t pkt[64];
    int actual_length;
    while(1) {
      int r = libusb_bulk_transfer(h, ep_addr, pkt, sizeof(pkt),
                                   &actual_length, 0);
      if(r) {
        if(r == LIBUSB_ERROR_PIPE)
          continue;
        break;
      }

      if(sc_handle_pkt(&recv, pkt, actual_length)) {
        // Capture complete
        sc_store_capture(st, &recv);
      }
    }

    libusb_release_interface(h, iface_num);
    libusb_close(h);
    free(recv.data);
    recv.data = NULL;
    sleep(1);
  }
  return NULL;
}

// --- MCP tool: sigcapture_list ---

static cJSON *
tool_sigcapture_list(mcp_context_t *ctx, const cJSON *params,
                     const char **errstr)
{
  (void)ctx;
  (void)params;

  pthread_mutex_lock(&sc_state.mutex);

  time_t now = time(NULL);
  cJSON *arr = cJSON_CreateArray();

  for(int i = sc_state.num_captures - 1; i >= 0; i--) {
    sc_capture_t *cap = &sc_state.captures[i];
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", cap->id);
    cJSON_AddNumberToObject(obj, "age_seconds", (double)(now - cap->timestamp));
    cJSON_AddNumberToObject(obj, "sample_rate", cap->sample_rate);
    cJSON_AddNumberToObject(obj, "trigger_offset", cap->trig_offset);
    cJSON_AddNumberToObject(obj, "depth", cap->depth);
    cJSON_AddNumberToObject(obj, "num_channels", cap->num_channels);

    cJSON *ch_arr = cJSON_CreateArray();
    for(int c = 0; c < cap->num_channels; c++) {
      cJSON *ch = cJSON_CreateObject();
      cJSON_AddStringToObject(ch, "name", cap->channels[c].name);
      cJSON_AddStringToObject(ch, "unit", cap->channels[c].unit);
      if(cap->channels[c].digital)
        cJSON_AddBoolToObject(ch, "digital", 1);
      cJSON_AddItemToArray(ch_arr, ch);
    }
    cJSON_AddItemToObject(obj, "channels", ch_arr);
    cJSON_AddItemToArray(arr, obj);
  }

  pthread_mutex_unlock(&sc_state.mutex);

  char *json_str = cJSON_Print(arr);
  cJSON_Delete(arr);
  cJSON *result = mcp_text_result(json_str);
  free(json_str);
  return result;
}

// --- MCP tool: sigcapture_save ---

static cJSON *
tool_sigcapture_save(mcp_context_t *ctx, const cJSON *params,
                     const char **errstr)
{
  (void)ctx;

  const cJSON *id_param = cJSON_GetObjectItem(params, "id");
  const cJSON *path_param = cJSON_GetObjectItem(params, "path");

  if(!cJSON_IsNumber(id_param)) {
    *errstr = "Missing required parameter: id";
    return NULL;
  }
  if(!cJSON_IsString(path_param)) {
    *errstr = "Missing required parameter: path";
    return NULL;
  }

  uint32_t target_id = (uint32_t)id_param->valuedouble;

  pthread_mutex_lock(&sc_state.mutex);

  sc_capture_t *cap = NULL;
  for(int i = 0; i < sc_state.num_captures; i++) {
    if(sc_state.captures[i].id == target_id) {
      cap = &sc_state.captures[i];
      break;
    }
  }

  if(!cap) {
    pthread_mutex_unlock(&sc_state.mutex);
    *errstr = "Capture ID not found";
    return NULL;
  }

  FILE *f = fopen(path_param->valuestring, "w");
  if(!f) {
    pthread_mutex_unlock(&sc_state.mutex);
    *errstr = "Failed to open output file";
    return NULL;
  }

  // Header
  fprintf(f, "time_s");
  for(int c = 0; c < cap->num_channels; c++) {
    fprintf(f, ",%s (%s)", cap->channels[c].name, cap->channels[c].unit);
  }
  fprintf(f, "\n");

  // Data rows
  for(int s = 0; s < cap->depth; s++) {
    double t = (double)(s - cap->trig_offset) / (double)cap->sample_rate;
    fprintf(f, "%.9f", t);
    for(int c = 0; c < cap->num_channels; c++) {
      fprintf(f, ",%.6g", cap->data[s * cap->num_channels + c]);
    }
    fprintf(f, "\n");
  }

  fclose(f);

  int depth = cap->depth;
  int nch = cap->num_channels;
  pthread_mutex_unlock(&sc_state.mutex);

  return mcp_text_resultf("Saved capture %u to %s (%d samples, %d channels)",
                          target_id, path_param->valuestring, depth, nch);
}

// --- Init ---

static cJSON *list_schema;
static cJSON *save_schema;

void
mcp_tool_sigcapture_init(mcp_context_t *ctx)
{
  // Configure background thread
  sc_state.usb = ctx->usb;
  sc_state.vid = ctx->usb_vid;
  sc_state.pid = ctx->usb_pid;
  sc_state.subclass = 192;
  pthread_mutex_init(&sc_state.mutex, NULL);

  // Start background capture thread
  pthread_create(&sc_state.thread, NULL, sc_bg_thread, &sc_state);
  pthread_detach(sc_state.thread);

  // sigcapture_list tool
  list_schema = cJSON_Parse(
    "{"
    "  \"type\": \"object\","
    "  \"properties\": {}"
    "}");

  static mcp_tool_t list_tool = {
    .name = "sigcapture_list",
    .description = "List recent signal captures stored in memory. "
      "Returns metadata for up to 16 most recent captures including "
      "capture ID, age, sample rate, depth, and channel names/units. "
      "No sample data is returned — use sigcapture_save to export "
      "a capture to CSV for analysis.",
    .handler = tool_sigcapture_list,
  };
  list_tool.input_schema = list_schema;
  mcp_register_tool(&list_tool);

  // sigcapture_save tool
  save_schema = cJSON_Parse(
    "{"
    "  \"type\": \"object\","
    "  \"properties\": {"
    "    \"id\": {"
    "      \"type\": \"integer\","
    "      \"description\": \"Capture ID from sigcapture_list\""
    "    },"
    "    \"path\": {"
    "      \"type\": \"string\","
    "      \"description\": \"Output CSV file path\""
    "    }"
    "  },"
    "  \"required\": [\"id\", \"path\"]"
    "}");

  static mcp_tool_t save_tool = {
    .name = "sigcapture_save",
    .description = "Save a signal capture to a CSV file. "
      "The CSV has a time_s column (relative to trigger) followed "
      "by one column per channel with scaled values. "
      "Suitable for analysis with Python/pandas/matplotlib.",
    .handler = tool_sigcapture_save,
  };
  save_tool.input_schema = save_schema;
  mcp_register_tool(&save_tool);
}
