// dsig over BLE: exposes the local dsig domain to a BLE client as the named
// service "dsig". One SDU = one signal frame: [group:16 LE][payload].
//
// The endpoint is a netif, so the existing dsig flooding fabric handles all
// routing: frames from the host enter dsig_input() and propagate to local
// subscribers and other interfaces (UART/CAN); locally emitted or forwarded
// signals reach the host through ni_dsig_output.
//
// Signals are periodically re-driven by their publishers, so this transport
// adds no reliability: on backpressure the oldest queued frame is dropped.

#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include <mios/service.h>
#include <mios/pushpull.h>
#include <mios/dsig.h>
#include <mios/cli.h>

#include "net/netif.h"
#include "net/pbuf.h"
#include "net/dsig.h"

#define BLE_DSIG_TXQ_MAX 8

LIST_HEAD(ble_dsig_client_list, ble_dsig_client);

typedef struct ble_dsig_client {
  LIST_ENTRY(ble_dsig_client) bdc_link;
  pushpull_t *bdc_pp;
  struct pbuf_queue bdc_txq;
  uint8_t bdc_txq_len;
} ble_dsig_client_t;

typedef struct ble_dsig {
  netif_t bd_ni;
  struct ble_dsig_client_list bd_clients;
} ble_dsig_t;

static ble_dsig_t g_ble_dsig;


static void
client_enqueue(ble_dsig_client_t *bdc, pbuf_t *pb)
{
  if(bdc->bdc_txq_len == BLE_DSIG_TXQ_MAX) {
    // Full: drop the oldest frame, the new one carries fresher state
    pbuf_free(pbuf_splice(&bdc->bdc_txq));
    bdc->bdc_txq_len--;
  }
  STAILQ_INSERT_TAIL(&bdc->bdc_txq, pb, pb_link);
  bdc->bdc_txq_len++;
  pushpull_wakeup(bdc->bdc_pp, PUSHPULL_EVENT_PULL);
}


// Signal from the dsig fabric towards the BLE clients. Runs on the net task.
static pbuf_t *
ble_dsig_output(struct netif *ni, pbuf_t *pb, uint32_t group, uint32_t flags)
{
  ble_dsig_t *bd = (ble_dsig_t *)ni;

  if(LIST_FIRST(&bd->bd_clients) == NULL)
    return pb; // no clients attached

  pb = pbuf_prepend(pb, 2, 0, 0);
  if(pb == NULL)
    return pb;

  uint8_t *hdr = pbuf_data(pb, 0);
  hdr[0] = group;
  hdr[1] = group >> 8;

  ble_dsig_client_t *bdc;
  LIST_FOREACH(bdc, &bd->bd_clients, bdc_link) {
    if(LIST_NEXT(bdc, bdc_link) != NULL) {
      pbuf_t *copy = pbuf_copy(pb, 0);
      if(copy != NULL)
        client_enqueue(bdc, copy);
    } else {
      client_enqueue(bdc, pb);
    }
  }
  return NULL;
}


// Frame from a BLE client into the dsig fabric.
static uint32_t
ble_dsig_push(void *opaque, struct pbuf *pb)
{
  ble_dsig_client_t *self = opaque;

  if(pb->pb_pktlen < 2) {
    pbuf_free(pb);
    return 0;
  }

  // The CoC channels are point-to-point, so the shared medium between BLE
  // clients is emulated here: fan out to every sibling except the origin
  // (dsig's split-horizon covers the netif level, not individual channels).
  ble_dsig_client_t *bdc;
  LIST_FOREACH(bdc, &g_ble_dsig.bd_clients, bdc_link) {
    if(bdc == self)
      continue;
    pbuf_t *copy = pbuf_copy(pb, 0);
    if(copy != NULL)
      client_enqueue(bdc, copy);
  }

  const uint8_t *hdr = pbuf_cdata(pb, 0);
  const uint32_t group = hdr[0] | (hdr[1] << 8);
  pb = pbuf_drop(pb, 2, 0);

  pb = dsig_input(group, pb, &g_ble_dsig.bd_ni);
  if(pb != NULL)
    pbuf_free(pb);
  return 0;
}


static int
ble_dsig_may_push(void *opaque)
{
  return 1;
}


static pbuf_t *
ble_dsig_pull(void *opaque)
{
  ble_dsig_client_t *bdc = opaque;

  pbuf_t *pb = pbuf_splice(&bdc->bdc_txq);
  if(pb != NULL)
    bdc->bdc_txq_len--;
  return pb;
}


static void
ble_dsig_close(void *opaque, const char *reason)
{
  ble_dsig_client_t *bdc = opaque;

  LIST_REMOVE(bdc, bdc_link);
  pbuf_free_queue_irq_blocked(&bdc->bdc_txq);
  free(bdc);
}


static const pushpull_app_fn_t ble_dsig_fns = {
  .push = ble_dsig_push,
  .may_push = ble_dsig_may_push,
  .pull = ble_dsig_pull,
  .close = ble_dsig_close,
};


static error_t
ble_dsig_open(pushpull_t *pp)
{
  ble_dsig_client_t *bdc = xalloc(sizeof(ble_dsig_client_t), 0,
                                  MEM_MAY_FAIL | MEM_CLEAR);
  if(bdc == NULL)
    return ERR_NO_MEMORY;

  STAILQ_INIT(&bdc->bdc_txq);
  bdc->bdc_pp = pp;
  LIST_INSERT_HEAD(&g_ble_dsig.bd_clients, bdc, bdc_link);
  pp->app = &ble_dsig_fns;
  pp->app_opaque = bdc;
  return 0;
}

SERVICE_DEF_PUSHPULL("dsig", 0, 0, ble_dsig_open);


static const device_class_t ble_dsig_device_class = {
  .dc_class_name = "dsig",
};

static void __attribute__((constructor(810)))
ble_dsig_init(void)
{
  ble_dsig_t *bd = &g_ble_dsig;

  LIST_INIT(&bd->bd_clients);
  bd->bd_ni.ni_dsig_output = ble_dsig_output;
  netif_init(&bd->bd_ni, "bledsig", &ble_dsig_device_class);
  netif_attach(&bd->bd_ni);
}


// Emit a signal from the CLI, mostly for testing the fabric end to end.
static error_t
cmd_dsigemit(cli_t *cli, int argc, char **argv)
{
  if(argc < 3)
    return ERR_INVALID_ARGS;
  uint32_t group = atoix(argv[1]);
  dsig_emit(group, argv[2], strlen(argv[2]));
  return 0;
}

CLI_CMD_DEF("dsigemit", cmd_dsigemit);
