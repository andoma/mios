#pragma once

#include <stddef.h>
#include <stdint.h>

#include <mios/error.h>

struct stream;

// DCMIPP camera pixel pipeline, CSI-2 input only.
//
// Pipe0 is used for raw snapshots (debugging), Pipe1 runs the ISP
// (black level, white balance gains, demosaicing, color matrix, gamma)
// and outputs double buffered NV12.

void stm32n6_dcmipp_init(void);

// Capture one raw frame (cropped) through Pipe0. RAW10 is unpacked to
// 16 bits per pixel, LSB aligned. `x` and `width` must be even.
error_t stm32n6_dcmipp_snapshot_raw(uint16_t *buf, int vc, int dt,
                                    int x, int y, int width, int height);

typedef struct {
  uint8_t vc;
  uint8_t dt;             // CSI data type (raw Bayer)
  uint8_t bayer;          // DCMIPP_BAYER_xxx
  uint8_t black_level;    // On 8 bit scale

  // Crop of the sensor frame, then downscaled to the output size
  uint16_t crop_x;
  uint16_t crop_y;
  uint16_t crop_width;
  uint16_t crop_height;

  uint16_t out_width;     // Multiple of 16 (it's also the pitch)
  uint16_t out_height;

  // 0: Whole frames, ping-pong between two frame buffers.
  // 16, 32 or 64: Stream into a ring of two halves with this many lines
  // each. The line event at every half is wired to the VENC (venc_rdy).
  uint8_t ring_lines;
} stm32n6_dcmipp_pipe1_config_t;

// Bytes needed for one output buffer: an NV12 frame (Y plane followed by
// the UV plane), or the ring (Y ring followed by the UV ring)
size_t stm32n6_dcmipp_buffer_size(const stm32n6_dcmipp_pipe1_config_t *cfg);

// Bytes from the start of a buffer to its UV plane / ring
size_t stm32n6_dcmipp_uv_offset(const stm32n6_dcmipp_pipe1_config_t *cfg);

// Start continuous capture. In frame mode into two buffers (ping-pong),
// in ring mode only `buf0` is used.
error_t stm32n6_dcmipp_pipe1_start(const stm32n6_dcmipp_pipe1_config_t *cfg,
                                   void *buf0, void *buf1);

void stm32n6_dcmipp_pipe1_stop(void);

// Wait for a frame newer than `*seq`. Returns the frame (Y plane, UV
// follows) and updates `*seq`, or NULL on timeout. The buffer is valid
// until the frame after the next one has started, i.e. the caller has
// one frame period to consume it.
void *stm32n6_dcmipp_pipe1_wait(uint32_t *seq, int64_t deadline);

// Line events since the last end of frame, i.e. how far into the next
// frame the DCMIPP is (ring mode)
uint32_t stm32n6_dcmipp_pipe1_lines_events(void);

// White balance / exposure gains in 1/256 units (256 = 1.0)
void stm32n6_dcmipp_set_gains(int r, int g, int b);

// 3x3 color correction matrix in 1/256 units, NULL disables
void stm32n6_dcmipp_set_ccm(const int16_t m[3][3]);

typedef struct {
  // Average raw R, G, B (8 bit scale, before black level and gains)
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint32_t seq;           // Frame the statistics belong to
} stm32n6_dcmipp_stats_t;

void stm32n6_dcmipp_get_stats(stm32n6_dcmipp_stats_t *st);

void stm32n6_dcmipp_print_status(struct stream *st);
