#pragma once

#include <stdint.h>

#include <mios/error.h>
#include <mios/stream.h>
#include <mios/image_sensor.h>
#include <mios/video_sink.h>

// Camera pipeline: sensor -> CSI-2 -> DCMIPP (ISP) -> VENC (H.264) -> sink
//
// The CSI, DCMIPP and VENC are handled as one unit, including auto
// exposure and white balance. There is a single instance.

typedef struct {
  uint16_t width;     // Encoded size, width multiple of 16
  uint16_t height;
  uint8_t qp;
  uint16_t gop;       // Frames between IDRs, 0 = one per second
  // Gradual intra refresh instead of periodic IDRs: frames per sweep of
  // the intra band, 0 = off (use `gop`)
  uint16_t intra_refresh;
  uint8_t slice_rows;   // Macroblock rows per slice, 0 = one slice
  // Stream from the ISP into the encoder 16 lines at a time instead of
  // going through whole frames in memory
  uint8_t streaming;
} stm32n6_camera_config_t;

// Bring up the CSI-2 receiver and the DCMIPP for `sensor` and start the
// sensor. Frames are only captured and encoded once started.
error_t stm32n6_camera_init(image_sensor_t *sensor);

// Start capturing and encoding into `sink`. The pipeline takes ownership
// of the sink and closes it when stopped.
error_t stm32n6_camera_start(const stm32n6_camera_config_t *cfg,
                             video_sink_t *sink);

void stm32n6_camera_stop(void);

// Status of the whole pipeline: sensor, CSI-2, ISP, encoder and sink
void stm32n6_camera_print_status(stream_t *st);

// Capture a raw crop of the sensor frame and print per Bayer site
// statistics (bring-up / debugging)
error_t stm32n6_camera_raw_stats(stream_t *st, int x, int y, int w, int h);

// Encoder settings, while running
error_t stm32n6_camera_set_qp(int qp);

error_t stm32n6_camera_set_slice_rows(int rows);

// Auto exposure / white balance
void stm32n6_camera_set_ae(int on);

void stm32n6_camera_set_awb(int on);

// Manual exposure (lines) and gain (256 = 1.0), turns auto exposure off
error_t stm32n6_camera_set_exposure(uint32_t lines, uint32_t gain);

// Manual white balance gains (256 = 1.0), turns auto white balance off
void stm32n6_camera_set_wb(int r, int b);

void stm32n6_camera_clear_stats(void);

// Print the encoder registers
void stm32n6_camera_dump_encoder(stream_t *st);
