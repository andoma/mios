#include "stm32n6_camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <mios/task.h>
#include <mios/eventlog.h>


#include "stm32n6_csi.h"
#include "stm32n6_csi_reg.h"
#include "stm32n6_dcmipp.h"
#include "stm32n6_venc.h"

// Auto exposure target for the mean green level (8 bit, linear, after
// black level). ST's IMX335 tuning uses 56.
#define AE_TARGET 56

// Exposure / gain changes take effect this many frames later
#define AE_INTERVAL 3

// Lines per half of the ISP -> encoder ring in streaming mode (one
// macroblock row)
#define RING_LINES 16

static struct {
  image_sensor_t *sensor;

  uint8_t black_level;    // 8 bit scale

  // Streaming
  thread_t *thread;
  volatile int run;
  stm32n6_camera_config_t cfg;
  stm32n6_dcmipp_pipe1_config_t isp;
  stm32n6_venc_h264_t *enc;
  video_sink_t *sink;
  uint8_t *frames[2];

  // Auto exposure / white balance
  uint8_t ae;
  uint8_t awb;
  uint32_t exposure;      // Lines
  uint32_t gain;          // Linear, 256 = 1.0
  uint32_t last_ae_seq;
  int wb_r;               // x256
  int wb_b;
  stm32n6_dcmipp_stats_t stats;

  // Rates, updated once per second
  uint32_t frames_total;
  uint32_t frames_window;
  uint32_t bytes_window;
  uint64_t window_start;
  uint32_t fps_x10;
  uint32_t kbps;
  uint32_t timeouts;
  uint32_t late;          // Streaming: frames missed, encoder armed too late
  uint32_t resyncs;       // Streaming: ISP / encoder desync recoveries
} cam;


/*
 * Auto exposure / white balance, run once per captured frame
 */

static void
camera_apply_exposure(void)
{
  image_sensor_t *is = cam.sensor;
  is->set_exposure(is, cam.exposure);
  is->set_gain(is, cam.gain);
}


static void
camera_ae(const stm32n6_dcmipp_stats_t *st)
{
  image_sensor_t *is = cam.sensor;

  if(st->seq - cam.last_ae_seq < AE_INTERVAL)
    return;
  cam.last_ae_seq = st->seq;

  int g = st->g - cam.black_level;
  if(g < 1)
    g = 1;

  // Total exposure in line units, scaled by the gain
  const uint64_t cur = (uint64_t)cam.exposure * cam.gain / 256;
  uint64_t want = cur * AE_TARGET / g;

  // Converge in steps, at most 2x per update, and leave it alone once
  // within ~6%
  if(want > cur * 2)
    want = cur * 2;
  if(want < cur / 2)
    want = cur / 2;
  if(want * 16 > cur * 15 && want * 16 < cur * 17)
    return;
  want = (want + cur) / 2;
  if(want < 1)
    want = 1;

  // Prefer long integration time over gain
  if(want <= is->max_exposure) {
    cam.exposure = want;
    cam.gain = 256;
  } else {
    cam.exposure = is->max_exposure;
    cam.gain = want * 256 / is->max_exposure;
    if(cam.gain > is->max_gain)
      cam.gain = is->max_gain;
  }
  camera_apply_exposure();
}


static int
clamp(int v, int lo, int hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}


static void
camera_awb(const stm32n6_dcmipp_stats_t *st)
{
  // Gray world on the raw (pre gain) averages
  const int r = st->r - cam.black_level;
  const int g = st->g - cam.black_level;
  const int b = st->b - cam.black_level;
  if(r < 4 || g < 4 || b < 4 || st->g > 240)
    return; // Too dark or saturated to say anything

  const int want_r = clamp(256 * g / r, 128, 1024);
  const int want_b = clamp(256 * g / b, 128, 1024);
  cam.wb_r = (cam.wb_r * 7 + want_r) / 8;
  cam.wb_b = (cam.wb_b * 7 + want_b) / 8;
  stm32n6_dcmipp_set_gains(cam.wb_r, 256, cam.wb_b);
}


static void
camera_3a(void)
{
  stm32n6_dcmipp_get_stats(&cam.stats);
  if(cam.ae)
    camera_ae(&cam.stats);
  if(cam.awb)
    camera_awb(&cam.stats);
}


/*
 * Pipeline
 */

static void
camera_account(size_t bytes)
{
  cam.frames_total++;
  cam.frames_window++;
  cam.bytes_window += bytes;
  const uint64_t now = clock_get();
  const uint64_t elapsed = now - cam.window_start;
  if(elapsed >= 1000000) {
    cam.fps_x10 = (uint64_t)cam.frames_window * 10000000 / elapsed;
    cam.kbps = (uint64_t)cam.bytes_window * 8000 / elapsed;
    cam.frames_window = 0;
    cam.bytes_window = 0;
    cam.window_start = now;
  }
}


static void
camera_output(const uint8_t *au, size_t len, uint64_t pts)
{
  // The encoder starts every IDR with SPS + PPS
  const int keyframe = len > 4 && (au[4] & 0x1f) == 7;
  cam.sink->frame(cam.sink, au, len, pts, keyframe);
  camera_account(len);
}


// Whole frames: the ISP ping-pongs between two frame buffers, and each
// completed frame is encoded from memory
static void
camera_loop_frames(void)
{
  const size_t uvoff = stm32n6_dcmipp_uv_offset(&cam.isp);
  uint32_t seq = 0;
  int first = 1;

  while(cam.run) {
    uint8_t *f = stm32n6_dcmipp_pipe1_wait(&seq, clock_get() + 500000);
    if(f == NULL) {
      cam.timeouts++;
      evlog(LOG_WARNING, "camera: No frame from the ISP");
      continue;
    }
    const uint64_t pts = clock_get();

    const uint8_t *au;
    size_t len;
    if(!stm32n6_venc_h264_encode(cam.enc, f, f + uvoff, first, &au, &len)) {
      camera_output(au, len, pts);
      first = 0;
    }
    camera_3a();
  }
}


// Streaming: the encoder is armed in the vertical blanking after a frame
// has ended and then encodes the next frame one macroblock row at a time,
// as the ISP signals that each half of the ring has been written
static void
camera_loop_streaming(void)
{
  uint8_t *ring = cam.frames[0];
  const size_t uvoff = stm32n6_dcmipp_uv_offset(&cam.isp);
  uint32_t seq = 0;
  int force_idr = 1;

  while(cam.run) {
    if(stm32n6_dcmipp_pipe1_wait(&seq, clock_get() + 500000) == NULL) {
      cam.timeouts++;
      evlog(LOG_WARNING, "camera: No frame from the ISP");
      continue;
    }

    camera_3a();

    // If the next frame has already started arriving it's too late to
    // arm the encoder for it, it would come out of sync with the ring
    if(stm32n6_dcmipp_pipe1_lines_events()) {
      cam.late++;
      continue;
    }

    const uint8_t *au;
    size_t len;
    error_t err = stm32n6_venc_h264_encode(cam.enc, ring, ring + uvoff,
                                           force_idr, &au, &len);
    if(err) {
      // ES0620 2.2.15: the encoder and the ISP can lose sync. Recover by
      // restarting the ISP (the encoder has been reset already).
      cam.resyncs++;
      stm32n6_dcmipp_pipe1_stop();
      stm32n6_dcmipp_pipe1_start(&cam.isp, ring, NULL);
      seq = 0;
      force_idr = 1;
      continue;
    }
    camera_output(au, len, clock_get());
    force_idr = 0;
  }
}


static void *
camera_thread(void *arg)
{
  cam.window_start = clock_get();
  if(cam.cfg.streaming)
    camera_loop_streaming();
  else
    camera_loop_frames();
  return NULL;
}


error_t
stm32n6_camera_start(const stm32n6_camera_config_t *cfg, video_sink_t *sink)
{
  image_sensor_t *is = cam.sensor;

  if(is == NULL || cam.thread != NULL) {
    sink->close(sink);
    return is == NULL ? ERR_NO_DEVICE : ERR_BAD_STATE;
  }

  cam.cfg = *cfg;
  if(cam.cfg.gop == 0)
    cam.cfg.gop = is->fps;

  // Crop the sensor frame to the output aspect ratio (centered), and let
  // the ISP scale it down
  uint32_t cw = is->width;
  uint32_t ch = is->width * cfg->height / cfg->width;
  if(ch > is->height) {
    ch = is->height;
    cw = is->height * cfg->width / cfg->height;
  }
  cw &= ~1;
  ch &= ~1;

  cam.isp = (stm32n6_dcmipp_pipe1_config_t) {
    .vc = 0,
    .dt = is->csi_data_type,
    .bayer = is->bayer,
    .black_level = cam.black_level,
    .crop_x = ((is->width - cw) / 2) & ~1,
    .crop_y = ((is->height - ch) / 2) & ~1,
    .crop_width = cw,
    .crop_height = ch,
    .out_width = cfg->width,
    .out_height = cfg->height,
    .ring_lines = cfg->streaming ? RING_LINES : 0,
  };

  // Captured frames go in the DMA heap. The encoder's reference frames
  // are only touched by the VENC, they go in AXISRAM3-6 (general heap).
  const stm32n6_venc_h264_config_t venc_cfg = {
    .width = cfg->width,
    .height = cfg->height,
    .fps = is->fps,
    .qp = cfg->qp,
    .gop = cam.cfg.gop,
    .intra_refresh = cfg->intra_refresh,
    .slice_rows = cfg->slice_rows,
    .ref_mem_type = 0,
    .stream_mb_rows = cfg->streaming ? RING_LINES / 16 : 0,
  };

  // Two frame buffers, or one ring in streaming mode
  const size_t size = stm32n6_dcmipp_buffer_size(&cam.isp);
  cam.frames[0] = xalloc(size, 64, MEM_TYPE_DMA);
  cam.frames[1] = cfg->streaming ? NULL : xalloc(size, 64, MEM_TYPE_DMA);
  cam.enc = stm32n6_venc_h264_create(&venc_cfg);

  error_t err = 0;
  if(cam.frames[0] == NULL || cam.enc == NULL ||
     (!cfg->streaming && cam.frames[1] == NULL))
    err = ERR_NO_MEMORY;
  else
    err = stm32n6_dcmipp_pipe1_start(&cam.isp, cam.frames[0], cam.frames[1]);

  if(err) {
    if(cam.enc)
      stm32n6_venc_h264_destroy(cam.enc);
    free(cam.frames[0]);
    free(cam.frames[1]);
    cam.enc = NULL;
    sink->close(sink);
    return err;
  }

  cam.sink = sink;
  cam.frames_total = 0;
  cam.frames_window = 0;
  cam.bytes_window = 0;
  cam.fps_x10 = 0;
  cam.kbps = 0;
  cam.timeouts = 0;
  cam.late = 0;
  cam.resyncs = 0;
  cam.run = 1;
  cam.thread = thread_create(camera_thread, NULL, 2048, "camera", 0, 4);
  return 0;
}


void
stm32n6_camera_stop(void)
{
  if(cam.thread == NULL)
    return;
  cam.run = 0;
  thread_join(cam.thread);
  cam.thread = NULL;

  stm32n6_dcmipp_pipe1_stop();
  stm32n6_venc_h264_destroy(cam.enc);
  cam.enc = NULL;
  free(cam.frames[0]);
  free(cam.frames[1]);
  cam.sink->close(cam.sink);
  cam.sink = NULL;
}


static int
csi_format_for_bits(int bits)
{
  switch(bits) {
  case 8:  return CSI_FT_BPP8;
  case 12: return CSI_FT_BPP12;
  case 14: return CSI_FT_BPP14;
  default: return CSI_FT_BPP10;
  }
}


error_t
stm32n6_camera_init(image_sensor_t *is)
{
  stm32n6_dcmipp_init();

  error_t err = stm32n6_csi_init(is->csi_lanes, is->csi_lane_mbps);
  if(err)
    return err;
  stm32n6_csi_config_vc(0, is->csi_data_type, csi_format_for_bits(is->bits));
  stm32n6_csi_start(0);

  cam.sensor = is;
  cam.black_level = is->black_level >> (is->bits - 8);
  cam.ae = 1;
  cam.awb = 1;
  cam.wb_r = 256;
  cam.wb_b = 256;
  cam.exposure = is->max_exposure / 2;
  cam.gain = 256;
  camera_apply_exposure();

  return is->start(is);
}


/*
 * Status and control
 */

static const char *
onoff(int v)
{
  return v ? "on" : "off";
}


void
stm32n6_camera_print_status(stream_t *st)
{
  image_sensor_t *is = cam.sensor;
  if(is == NULL) {
    stprintf(st, "No camera\n");
    return;
  }

  if(cam.thread) {
    stprintf(st, "Pipeline: running %dx%d, %s mode, %u.%u fps, %u kbit/s\n",
             cam.cfg.width, cam.cfg.height,
             cam.cfg.streaming ? "streaming" : "frame",
             cam.fps_x10 / 10, cam.fps_x10 % 10, cam.kbps);
    stprintf(st, "  Frames:%u  ISP timeouts:%u", cam.frames_total,
             cam.timeouts);
    if(cam.cfg.streaming)
      stprintf(st, "  Late:%u  Resyncs:%u", cam.late, cam.resyncs);
    stprintf(st, "\n");
  } else {
    stprintf(st, "Pipeline: idle\n");
  }

  static const char bayer[4][5] = { "RGGB", "GRBG", "GBRG", "BGGR" };
  stprintf(st, "\nSensor: %s %dx%d RAW%d %s @ %d fps, "
           "%d lane%s x %d Mbps, black level %d\n",
           is->name, is->width, is->height, is->bits, bayer[is->bayer & 3],
           is->fps, is->csi_lanes, is->csi_lanes > 1 ? "s" : "",
           is->csi_lane_mbps, is->black_level);
  is->print_info(is, st);
  stprintf(st, "  AE: %s (target %d)  AWB: %s (R x%d.%02d  B x%d.%02d)\n",
           onoff(cam.ae), AE_TARGET, onoff(cam.awb),
           cam.wb_r / 256, cam.wb_r % 256 * 100 / 256,
           cam.wb_b / 256, cam.wb_b % 256 * 100 / 256);

  stprintf(st, "\nCSI-2:\n");
  stm32n6_csi_print_status(st);

  stprintf(st, "\nISP (DCMIPP):\n");
  if(cam.thread) {
    stprintf(st, "  Crop %dx%d+%d+%d -> %dx%d NV12, ",
             cam.isp.crop_width, cam.isp.crop_height,
             cam.isp.crop_x, cam.isp.crop_y,
             cam.isp.out_width, cam.isp.out_height);
    if(cam.cfg.streaming)
      stprintf(st, "%d line ring at %p (%d bytes)\n", cam.isp.ring_lines * 2,
               cam.frames[0], (int)stm32n6_dcmipp_buffer_size(&cam.isp));
    else
      stprintf(st, "frames at %p %p (%d bytes each)\n",
               cam.frames[0], cam.frames[1],
               (int)stm32n6_dcmipp_buffer_size(&cam.isp));
  }
  stm32n6_dcmipp_print_status(st);

  stprintf(st, "\nEncoder (VENC):\n");
  if(cam.enc)
    stm32n6_venc_h264_print_status(cam.enc, st);
  else
    stprintf(st, "  Not running\n");

  stprintf(st, "\nOutput:\n");
  if(cam.sink)
    cam.sink->print_info(cam.sink, st);
  else
    stprintf(st, "  None\n");
}


error_t
stm32n6_camera_raw_stats(stream_t *st, int x, int y, int w, int h)
{
  image_sensor_t *is = cam.sensor;
  if(is == NULL)
    return ERR_NO_DEVICE;

  x &= ~1;
  w &= ~1;
  uint16_t *buf = xalloc(w * h * 2, 64, MEM_TYPE_DMA);
  if(buf == NULL)
    return ERR_NO_MEMORY;

  error_t err = stm32n6_dcmipp_snapshot_raw(buf, 0, is->csi_data_type,
                                            x, y, w, h);
  if(err) {
    free(buf);
    return err;
  }

  uint32_t sum[4] = {}, min[4], max[4] = {};
  for(int i = 0; i < 4; i++)
    min[i] = 0xffff;
  for(int r = 0; r < h; r++) {
    for(int c = 0; c < w; c++) {
      const int p = (r & 1) * 2 + (c & 1);
      const uint16_t v = buf[r * w + c];
      sum[p] += v;
      if(v < min[p]) min[p] = v;
      if(v > max[p]) max[p] = v;
    }
  }
  const int n = w * h / 4;
  for(int p = 0; p < 4; p++) {
    stprintf(st, "Phase %d%d: avg %4d  min %4d  max %4d\n",
             p >> 1, p & 1, (int)(sum[p] / n), (int)min[p], (int)max[p]);
  }
  free(buf);
  return 0;
}


error_t
stm32n6_camera_set_qp(int qp)
{
  if(cam.enc == NULL)
    return ERR_BAD_STATE;
  stm32n6_venc_h264_set_qp(cam.enc, qp);
  return 0;
}


error_t
stm32n6_camera_set_slice_rows(int rows)
{
  if(cam.enc == NULL)
    return ERR_BAD_STATE;
  stm32n6_venc_h264_set_slice_rows(cam.enc, rows);
  return 0;
}


void
stm32n6_camera_set_ae(int on)
{
  cam.ae = on;
}


void
stm32n6_camera_set_awb(int on)
{
  cam.awb = on;
}


error_t
stm32n6_camera_set_exposure(uint32_t lines, uint32_t gain)
{
  if(cam.sensor == NULL)
    return ERR_NO_DEVICE;
  cam.ae = 0;
  cam.exposure = lines;
  cam.gain = gain;
  camera_apply_exposure();
  return 0;
}


void
stm32n6_camera_set_wb(int r, int b)
{
  cam.awb = 0;
  cam.wb_r = r;
  cam.wb_b = b;
  stm32n6_dcmipp_set_gains(cam.wb_r, 256, cam.wb_b);
}


void
stm32n6_camera_clear_stats(void)
{
  stm32n6_csi_clear_stats();
}


void
stm32n6_camera_dump_encoder(stream_t *st)
{
  stm32n6_venc_dump_regs(st);
}
