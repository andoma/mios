#pragma once

#include <stddef.h>
#include <stdint.h>

#include "error.h"

struct stream;

// Consumer of an encoded video stream

typedef struct video_sink {

  // One H.264 access unit in Annex B format. `pts` is the capture time in
  // microseconds (clock_get()). The data is only valid during the call.
  error_t (*frame)(struct video_sink *vs, const uint8_t *data, size_t len,
                   uint64_t pts, int keyframe);

  void (*print_info)(struct video_sink *vs, struct stream *st);

  void (*close)(struct video_sink *vs);

} video_sink_t;
