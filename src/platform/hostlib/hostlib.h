#pragma once
/*
 * What the hostlib platform offers an application's board file: the
 * virtual buses a harness drives through the ABI in libmios.h. A hostlib
 * board file creates its drivers on these exactly like a real board file
 * does on stm32h7_spi_create() and friends.
 */
#include <mios/io.h>

#include "vi2c.h"
#include "vspi.h"

struct vcan;

/* The virtual CAN interface (a real can_netif, so DSIG/VLLP run
   unmodified). Created and brought up on first call; call it from main()
   so the harness has something to talk to. MTU 64 (FDCAN). */
struct vcan *hostlib_can(void);
