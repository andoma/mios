# STM32N6570-DK camera and H.264 streaming

Hardware: STM32N6570-DK (MB1939) with the B-CAMS-IMX camera module
(MB1854 adapter + Sony IMX335) on CN14.

## Pipeline

    image_sensor_t --CSI-2--> [ stm32n6 camera unit ] --H.264--> video_sink_t
                              CSI host -> DCMIPP -> VENC
                              + auto exposure / white balance

| Part                         | Code                                        |
|------------------------------|---------------------------------------------|
| Sensor interface             | `include/mios/image_sensor.h`               |
| IMX335 sensor driver         | `src/drivers/camera/imx335.c`               |
| Camera unit (CSI+ISP+VENC)   | `src/platform/stm32n6/stm32n6_camera.c`     |
| CSI-2 / DCMIPP / VENC        | `src/platform/stm32n6/stm32n6_{csi,dcmipp,venc}.c` |
| Encoded video interface      | `include/mios/video_sink.h`                 |
| RTP sink (RFC 6184, mode 1)  | `src/net/rtp_h264.c`                        |
| Board wiring, `camera` CLI   | `src/platform/stm32n6-dk/camera.c`          |

The sensor driver describes its output (size, Bayer order, bit depth,
black level, CSI-2 lanes and lane rate, exposure and gain limits) and
implements start/stop, exposure, gain and test pattern. The camera unit
configures the CSI-2 receiver from that, runs the ISP and the encoder,
and hands every encoded access unit to the sink.

The ISP feeds the encoder in one of two ways:

- Streaming (default): the DCMIPP writes into a ring of two 16 line
  halves, and the line event at the end of each half triggers the VENC
  (venc_rdy) to encode that macroblock row. The encoder is armed in the
  vertical blanking after each frame. Only ~1.5 x 32 lines of memory are
  needed and latency is lower, but the encoder must keep up in real time.
- Frame: the DCMIPP ping-pongs between two whole NV12 frames and each
  completed frame is encoded from memory.

## Receiving the stream

### camview

`host/camview` is a minimal viewer made for this stream. It takes RTP on a
UDP port directly (no SDP), decodes every frame as soon as its last packet
has arrived and shows it right away (no jitter buffer, no clock sync), so
it has much less latency than ffplay. A new SSRC or a large sequence
number jump resets it, so restarting the stream on the device is picked
up immediately.

    sudo apt install libavcodec-dev libavutil-dev libsdl2-dev
    make -C host/camview
    host/camview/camview [-p 5000] [-o] [-d pct]

The window title (and stdout, once a second) shows resolution, frame
rate, bitrate, I/P frame counts, slices per frame, the share of intra
macroblocks, packet loss and arrival jitter.

| Key | Action |
|-----|--------|
| `o` | Overlay: intra coded macroblocks in red |
| `v` | Motion vectors |
| `g` | Macroblock grid |
| `f` | Fullscreen |
| `1`-`5` | Simulate 1-5% packet loss (random drops before the receiver), `0` off |
| `q` | Quit |

With intra refresh on, the overlay shows the band sweeping down the
picture, plus whatever the encoder decided to code intra on its own.

### ffplay / GStreamer

ffplay needs an SDP file describing the stream. Save this as `stream.sdp`
(change the port if needed):

    v=0
    o=- 0 0 IN IP4 0.0.0.0
    s=mios
    c=IN IP4 0.0.0.0
    t=0 0
    m=video 5000 RTP/AVP 96
    a=rtpmap:96 H264/90000
    a=fmtp:96 packetization-mode=1

Then:

    ffplay -protocol_whitelist file,udp,rtp -fflags nobuffer -flags low_delay stream.sdp

or with GStreamer:

    gst-launch-1.0 udpsrc port=5000 caps="application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=96" ! \
        rtph264depay ! avdec_h264 ! autovideosink sync=false

On the device:

    camera start <host ip> 5000 [qp]
    camera stop

The stream begins with an IDR frame. After that it uses gradual intra
refresh by default: a band of intra coded macroblock rows sweeps down the
picture once per second, so there are no large IDR frames and the bitrate
stays even. SPS, PPS and a recovery point SEI are sent at the start of
every sweep, so a receiver can join at any time; the picture is complete
after one sweep. With `gop` an IDR (with SPS and PPS) is sent once per
second instead.

## Shell commands

Everything is under `camera`. Without arguments it prints the status of
the whole pipeline: sensor, CSI-2 receiver, ISP, encoder and output.

| Command                     | Purpose                                       |
|-----------------------------|-----------------------------------------------|
| `camera`                    | Pipeline status                               |
| `camera start <ip> <port> [WxH] [qp] [frame] [gop]` | Stream H.264 over RTP, default 640x480 (width a multiple of 16; 4:3 keeps the full sensor view, other aspect ratios crop). `frame` selects whole frame mode, `gop` periodic IDRs instead of intra refresh |
| `camera stop`               | Stop streaming                                |
| `camera qp <n>`             | Change the encoder QP                         |
| `camera slices <rows>`      | Macroblock rows per slice, 0 = one slice per frame. Each slice starts a new RTP packet, so a lost packet only damages its slice |
| `camera ae on\|off`         | Auto exposure                                 |
| `camera awb on\|off`        | Auto white balance                            |
| `camera exposure <lines> [gain]` | Manual exposure and gain (256 = 1.0), turns AE off |
| `camera wb <r> <b>`         | Manual white balance gains, x256 (turns AWB off) |
| `camera tpg <n\|-1>`        | Sensor test pattern                           |
| `camera raw [x y [w h]]`    | Raw capture of a crop, prints Bayer statistics |
| `camera clear`              | Clear the CSI-2 error counters                |
| `camera regs`               | Dump the encoder registers                    |

## Time-of-flight sensor

The camera module also carries a VL53L5CX multizone ToF sensor
(`src/drivers/vl53l5cx.c`) on the same I2C bus. It has no flash: at boot
the camera init thread uploads its 84 KB firmware, which takes a few
seconds. The firmware and default calibration blobs come from ST's ULD
driver (BSD-3-Clause).

| Command                     | Purpose                                       |
|-----------------------------|-----------------------------------------------|
| `tof start [4\|8] [hz]`      | Start ranging, default 8x8 zones at 15 Hz (4x4 goes up to 60 Hz) |
| `tof stop`                  | Stop ranging                                  |
| `tof [frames]`              | Print the distance grid for one or more frames |
