#include "camera.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <mios/io.h>
#include <mios/cli.h>
#include <mios/task.h>

#include <net/net.h>
#include <net/rtp_h264.h>

#include "drivers/camera/imx335.h"
#include "drivers/vl53l5cx.h"

#include "stm32n6_i2c.h"
#include "stm32n6_camera.h"

// Camera connector CN14 (MB1854 / B-CAMS-IMX module)
#define CAM_PWR_EN  GPIO_PD(2) // EN_MODULE: sensor 1.2V supply and 24 MHz clock
#define CAM_NRST    GPIO_PC(8) // NRST_CAM: sensor XCLR (active low)
#define CAM_I2C_SCL GPIO_PH(9)
#define CAM_I2C_SDA GPIO_PC(1)
// The VL53L5CX LPn (PQ5) is pulled up on the module and left alone,
// TOF_INT (PQ0) is not used, the driver polls

#define SENSOR_FPS 30

static image_sensor_t *g_sensor;
static vl53l5cx_t *g_tof;


static void *
camera_thread(void *arg)
{
  // Sensor reset is released once the module supply has settled
  // (timing as in ST's BSP)
  usleep(100000);
  gpio_set_output(CAM_NRST, 1);
  usleep(100000);

  image_sensor_t *sensor = imx335_create(i2c_get_bus(1), SENSOR_FPS);
  if(sensor != NULL && !stm32n6_camera_init(sensor))
    g_sensor = sensor;

  // The ToF firmware upload takes a few seconds, so it goes last
  g_tof = vl53l5cx_create(i2c_get_bus(1));
  return NULL;
}


void
camera_init(void)
{
  // Keep the sensor in reset while the module supply comes up
  gpio_conf_output(CAM_NRST, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_set_output(CAM_NRST, 0);
  gpio_conf_output(CAM_PWR_EN, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_set_output(CAM_PWR_EN, 1);

  // I2C1 is shared by the sensor, the IMU and the ToF on the camera module
  stm32n6_i2c_create(1, CAM_I2C_SCL, CAM_I2C_SDA, GPIO_PULL_NONE, 400000);

  thread_create(camera_thread, NULL, 2048, "caminit", TASK_DETACHED, 3);
}


static error_t
cmd_camera(cli_t *cli, int argc, char **argv)
{
  image_sensor_t *is = g_sensor;
  if(is == NULL) {
    cli_printf(cli, "No camera\n");
    return ERR_NO_DEVICE;
  }

  if(argc == 1) {
    stm32n6_camera_print_status(cli->cl_stream);
    return 0;
  }

  const char *cmd = argv[1];

  if(!strcmp(cmd, "start") && argc >= 4) {
    stm32n6_camera_config_t cfg = {
      .width = 640,
      .height = 480,
      .qp = 28,
      .streaming = 1,
      .intra_refresh = is->fps,
    };
    for(int i = 4; i < argc; i++) {
      const char *x = strchr(argv[i], 'x');
      if(x != NULL) {
        cfg.width = atoi(argv[i]);
        cfg.height = atoi(x + 1);
      } else if(!strcmp(argv[i], "frame"))
        cfg.streaming = 0;
      else if(!strcmp(argv[i], "gop"))
        cfg.intra_refresh = 0;
      else
        cfg.qp = atoi(argv[i]);
    }
    video_sink_t *sink = rtp_h264_create(inet_addr(argv[2]), atoi(argv[3]));
    return stm32n6_camera_start(&cfg, sink);
  }

  if(!strcmp(cmd, "stop")) {
    stm32n6_camera_stop();
    return 0;
  }

  if(!strcmp(cmd, "qp") && argc == 3)
    return stm32n6_camera_set_qp(atoi(argv[2]));

  if(!strcmp(cmd, "slices") && argc == 3)
    return stm32n6_camera_set_slice_rows(atoi(argv[2]));

  if(!strcmp(cmd, "ae") && argc == 3) {
    stm32n6_camera_set_ae(!strcmp(argv[2], "on"));
    return 0;
  }

  if(!strcmp(cmd, "awb") && argc == 3) {
    stm32n6_camera_set_awb(!strcmp(argv[2], "on"));
    return 0;
  }

  if(!strcmp(cmd, "exposure") && argc >= 3)
    return stm32n6_camera_set_exposure(atoi(argv[2]),
                                       argc >= 4 ? atoi(argv[3]) : 256);

  if(!strcmp(cmd, "wb") && argc == 4) {
    stm32n6_camera_set_wb(atoi(argv[2]), atoi(argv[3]));
    return 0;
  }

  if(!strcmp(cmd, "tpg") && argc == 3)
    return is->set_test_pattern(is, atoi(argv[2]));

  if(!strcmp(cmd, "raw")) {
    int x = is->width / 2 - 32, y = is->height / 2 - 32, w = 64, h = 64;
    if(argc >= 4) {
      x = atoi(argv[2]);
      y = atoi(argv[3]);
    }
    if(argc >= 6) {
      w = atoi(argv[4]);
      h = atoi(argv[5]);
    }
    return stm32n6_camera_raw_stats(cli->cl_stream, x, y, w, h);
  }

  if(!strcmp(cmd, "clear")) {
    stm32n6_camera_clear_stats();
    return 0;
  }

  if(!strcmp(cmd, "regs")) {
    stm32n6_camera_dump_encoder(cli->cl_stream);
    return 0;
  }

  return ERR_INVALID_ARGS;
}

CLI_CMD_DEF_EXT("camera", cmd_camera,
                "[start <ip> <port> [WxH] [qp] [frame] [gop] | stop | qp <n> | "
                "slices <rows> | ae on|off | awb on|off | "
                "exposure <lines> [gain] | wb <r> <b> | tpg <n|-1> | "
                "raw [x y [w h]] | clear | regs]",
                "Camera pipeline status and control");


static error_t
cmd_tof(cli_t *cli, int argc, char **argv)
{
  vl53l5cx_t *v = g_tof;
  if(v == NULL) {
    cli_printf(cli, "No ToF sensor\n");
    return ERR_NO_DEVICE;
  }

  if(argc >= 2 && !strcmp(argv[1], "start")) {
    const int side = argc >= 3 ? atoi(argv[2]) : 8;
    const int hz = argc >= 4 ? atoi(argv[3]) : (side == 8 ? 15 : 30);
    return vl53l5cx_start(v, side, hz);
  }

  if(argc == 2 && !strcmp(argv[1], "stop"))
    return vl53l5cx_stop(v);

  if(argc > 2)
    return ERR_INVALID_ARGS;

  if(!vl53l5cx_is_ranging(v)) {
    cli_printf(cli, "VL53L5CX idle\n");
    return 0;
  }

  const int count = argc == 2 ? atoi(argv[1]) : 1;
  vl53l5cx_frame_t *f = xalloc(sizeof(vl53l5cx_frame_t), 0, MEM_MAY_FAIL);
  if(f == NULL)
    return ERR_NO_MEMORY;

  error_t err = 0;
  for(int i = 0; i < count; i++) {
    if((err = vl53l5cx_read(v, f, clock_get() + 1000000)))
      break;
    vl53l5cx_print_frame(f, cli->cl_stream);
  }
  free(f);
  return err;
}

CLI_CMD_DEF_EXT("tof", cmd_tof,
                "[start [4|8] [hz] | stop | <frames>]",
                "VL53L5CX time-of-flight sensor");
