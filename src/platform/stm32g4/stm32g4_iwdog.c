#include <mios/sys.h>
#include <mios/io.h>
#include <mios/task.h>

#include "stm32g4_reg.h"

#include <unistd.h>

#define IWDG_KR  0x40003000
#define IWDG_PR  0x40003004
#define IWDG_RLR 0x40003008
#define IWDG_SR  0x4000300c

static void * __attribute__((noreturn))
wdog_thread(void *arg)
{
  gpio_t gpio = (gpio_t)(intptr_t)arg;


  while(1) {
    reg_wr(IWDG_KR, 0xAAAA);
    if(gpio != GPIO_UNUSED)
      gpio_set_output(gpio, 1);
    usleep(500000);
    reg_wr(IWDG_KR, 0xAAAA);
    if(gpio != GPIO_UNUSED)
      gpio_set_output(gpio, 0);
    usleep(500000);
  }

}



void
sys_watchdog_start(gpio_t blink)
{
  reg_wr(IWDG_KR, 0x5555);
  reg_wr(IWDG_RLR, 256); // 2 seconds
  reg_wr(IWDG_PR, 6);
  reg_wr(IWDG_KR, 0xAAAA);
  reg_wr(IWDG_KR, 0xCCCC);

  thread_create(wdog_thread, (void *)(intptr_t)blink, 256, "wdog", 0, 0);
}
