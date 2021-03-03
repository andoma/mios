#include <mios/task.h>
#include "pbuf.h"

#include "netif.h"

struct netif_list netifs;

mutex_t net_output_mutex = MUTEX_INITIALIZER("netout");



static void  __attribute__((constructor(190)))
net_main_init(void)
{
  pbuf_alloc(64);
}

