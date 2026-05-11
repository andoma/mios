#include <mios/vllp.h>

#define VLLP_TIMEOUT_SECONDS 3

static void __attribute__((constructor(800)))
vexpress_a9_vllp_init(void)
{
  /* MTU-64 server (FDCAN-sized payload) */
  vllp_server_create(0x200, 0x201, 64, VLLP_TIMEOUT_SECONDS);

  /* MTU-8 server (classic CAN-sized payload) */
  vllp_server_create(0x210, 0x211, 8, VLLP_TIMEOUT_SECONDS);
}
