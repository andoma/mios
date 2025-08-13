#include "tegra234-ccplex_bpmp.h"
#include "tegra234-ccplex_clk.h"

#include "tegra234_hsp.h"
#include "tegra234_bpmp.h"

#include <stdio.h>
#include <string.h>

#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#define BPMP_OPT_DO_ACK                     (1 << 0)
#define BPMP_OPT_RING_DOORBELL              (1 << 1)
#define BPMP_OPT_CRC                        (1 << 2)


typedef struct {
  uint32_t mrq;

  uint8_t options:4;
  uint8_t xid:4;
  uint8_t payload_length;
  uint16_t crc16;

  uint8_t data[120];

} bpmp_req_t;


typedef struct {
  uint32_t error;

  uint8_t options:4;
  uint8_t xid:4;
  uint8_t payload_length;
  uint16_t crc16;

  uint8_t data[120];

} bpmp_resp_t;


typedef struct {
  uint32_t wr_ptr;
  uint32_t state;
  uint8_t pad1[56];

  uint32_t rd_ptr;
  uint8_t pad2[60];

  uint8_t data[0];
} ivc_ring_t;


#define IVC_STATE_EST 0
#define IVC_STATE_SYN 1
#define IVC_STATE_ACK 2


static volatile ivc_ring_t *bpmp_tx = (volatile ivc_ring_t *)0x40070000;
static volatile ivc_ring_t *bpmp_rx = (volatile ivc_ring_t *)0x40071000;

static uint16_t
bpmp_crc(uint16_t crc, uint8_t *data, size_t size)
{
  for (size_t i = 0; i < size; i++) {
    crc ^= data[i] << 8;
    for (size_t j = 0; j < 8; j++) {
      if ((crc & 0x8000) == 0x8000) {
        crc = (crc << 1) ^ 0xAC9A;
      } else {
        crc = (crc << 1);
      }
    }
  }
  return crc;
}


#if 0
uint16_t calc_crc(uint8_t *data, size_t size)
{
	return calc_crc_digest(0x4657, data, size);
}
#endif


static void
bpmp_notify(void)
{
  asm volatile("dmb st");
  hsp_db_ring(NV_ADDRESS_MAP_TOP0_HSP_BASE, 3);
}

static void
bpmp_ivc_sync(void)
{
  int rounds = 0;
  printf("BPMP sync ... ");


  bpmp_tx->state = IVC_STATE_SYN;
  bpmp_notify();

  while(1) {
    rounds++;
    if(bpmp_rx->state == IVC_STATE_SYN) {
      asm volatile("dmb ld");
      bpmp_tx->wr_ptr = 0;
      bpmp_rx->rd_ptr = 0;
      asm volatile("dmb st");
      bpmp_tx->state = IVC_STATE_ACK;
      bpmp_notify();

    } else if(bpmp_tx->state == IVC_STATE_SYN &&
              bpmp_rx->state == IVC_STATE_ACK) {

      asm volatile("dmb ld");
      bpmp_tx->wr_ptr = 0;
      bpmp_rx->rd_ptr = 0;
      asm volatile("dmb st");
      bpmp_tx->state = IVC_STATE_EST;
      bpmp_notify();

    } else if(bpmp_tx->state == IVC_STATE_ACK) {
      asm volatile("dmb ld");
      bpmp_tx->state = IVC_STATE_EST;
      bpmp_notify();
    } else if(bpmp_tx->state == IVC_STATE_EST) {
      break;
    }
  }
  printf("OK\n");
}


struct bpmp_msg_dispatch {
  task_waitable_t waitq;
  mutex_t mutex;
  uint32_t n_irq;
  uint32_t n_spurious_rx;
  int wait_buf;
  bpmp_resp_t buf;
};


static struct bpmp_msg_dispatch g_bpmp_msg_dispatch;


static void
bpmp_doorbell_irq(void *arg)
{
  struct bpmp_msg_dispatch *d = arg;
  d->n_irq++;
  reg_wr(hsp_db_base(NV_ADDRESS_MAP_TOP0_HSP_BASE, 1) + 0xc, ~0);

  asm volatile("dmb ld");
  if(bpmp_rx->wr_ptr != bpmp_rx->rd_ptr) {
    if(d->wait_buf) {
      memcpy(&d->buf, (void *)0x40071080, 128);
      d->wait_buf = 0;
      task_wakeup(&d->waitq, 0);
    } else {
      d->n_spurious_rx++;
    }

    bpmp_rx->rd_ptr++;
    bpmp_notify();
  }
}


static void __attribute__((constructor(200)))
bpmp_init(void)
{
  hsp_db_enable(NV_ADDRESS_MAP_TOP0_HSP_BASE, 1, 3, 1);

  bpmp_ivc_sync();

  struct bpmp_msg_dispatch *d = &g_bpmp_msg_dispatch;

  task_waitable_init(&d->waitq, "bpmp");
  mutex_init(&d->mutex, "bpmp");

  uint32_t en = hsp_connect_irq(NV_ADDRESS_MAP_TOP0_HSP_BASE, bpmp_doorbell_irq,
                                d, HSP_IRQ_DOORBELL(1));

  reg_set_bit(en, HSP_IRQ_DOORBELL(1));
}



#include <mios/cli.h>

static error_t
cmd_bpmp(cli_t *cli, int argc, char **argv)
{
  struct bpmp_msg_dispatch *d = &g_bpmp_msg_dispatch;

  cli_printf(cli, "IRQ:%u  spurious:%u\n",
             d->n_irq, d->n_spurious_rx);

  cli_printf(cli, "TX wr_ptr:0x%x  rd_ptr:%x  state:%d\n",
             bpmp_tx->wr_ptr, bpmp_tx->rd_ptr, bpmp_tx->state);

  cli_printf(cli, "RX wr_ptr:0x%x  rd_ptr:%x  state:%d\n",
             bpmp_rx->wr_ptr, bpmp_rx->rd_ptr, bpmp_rx->state);

  return 0;
}

CLI_CMD_DEF("bpmp", cmd_bpmp);

#define BPMP_EPERM	1
#define BPMP_ENOENT	2
#define BPMP_ENOHANDLER	3
#define BPMP_EIO	5
#define BPMP_EBADCMD	6
#define BPMP_EAGAIN	11
#define BPMP_ENOMEM	12
#define BPMP_EACCES	13
#define BPMP_EFAULT	14
#define BPMP_EBUSY	16
#define BPMP_ENODEV	19
#define BPMP_EISDIR	21
#define BPMP_EINVAL	22
#define BPMP_ETIMEDOUT  23
#define BPMP_ERANGE	34
#define BPMP_ENOSYS	38
#define BPMP_EBADSLT	57
#define BPMP_EBADMSG	77
#define BPMP_EOPNOTSUPP 95
#define BPMP_ENAVAIL	119
#define BPMP_ENOTSUP	134
#define BPMP_ENXIO	140


error_t
bpmp_xfer(uint32_t mrq,
          void *in, size_t in_size,
          void *out, size_t *out_size)
{
  struct bpmp_msg_dispatch *d = &g_bpmp_msg_dispatch;

  if(in_size > 120)
    return ERR_MTU_EXCEEDED;

  bpmp_req_t tmp;
  tmp.mrq = mrq;
  tmp.options = BPMP_OPT_DO_ACK | BPMP_OPT_RING_DOORBELL | BPMP_OPT_CRC;
  tmp.xid = 0;
  tmp.crc16 = 0;
  tmp.payload_length = in_size;
  memcpy(tmp.data, in, in_size);
  tmp.crc16 = bpmp_crc(0x4657, (void *)&tmp, in_size + 8);

  mutex_lock(&d->mutex);

  bpmp_req_t *tx = (bpmp_req_t *)0x40070080;
  memcpy(tx, &tmp, in_size + 8);

  int q = irq_forbid(IRQ_LEVEL_IO);
  d->wait_buf = 1;

  asm volatile("dmb st");
  bpmp_tx->wr_ptr++;
  bpmp_notify();

  while(d->wait_buf) {
    task_sleep(&d->waitq);
  }
  irq_permit(q);

  uint16_t sent_crc = d->buf.crc16;
  d->buf.crc16 = 0;
  uint16_t calc_crc = bpmp_crc(0x4657, (void *)&d->buf, d->buf.payload_length + 8);

  error_t err = 0;

  if(sent_crc != calc_crc) {
    err = ERR_CHECKSUM_ERROR;
    goto out;
  }

  if(d->buf.payload_length > 120) {
    err = ERR_MALFORMED;
    goto out;
  }

  if(d->buf.error) {
    // Fixme: Translate BPMP errors
    err = ERR_OPERATION_FAILED;
    goto out;
  }

  if(out && out_size) {
    if(d->buf.payload_length > *out_size) {
      err = ERR_BAD_PKT_SIZE;
      goto out;
    }

    *out_size = d->buf.payload_length;
    memcpy(out, &d->buf.data, d->buf.payload_length);
  }

 out:
  mutex_unlock(&d->mutex);
  return err;
}




#include <stdlib.h>
#include <unistd.h>

static error_t
cmd_ping(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  int val = atoi(argv[1]);

  uint32_t resp;
  size_t resp_size = sizeof(resp);
  error_t err = bpmp_xfer(9, &val, sizeof(val), &resp, &resp_size);
  if(err) {
    cli_printf(cli, "BPMP error %d\n", err);
    return 0;
  }
  if(err)
    return err;

  cli_printf(cli, "response:%d\n", resp);
  return 0;
}


CLI_CMD_DEF("ping", cmd_ping);

static error_t
cmd_clocks(cli_t *cli, int argc, char **argv)
{
  struct bpmp_mrq_clk_req req;
  struct bpmp_mrq_clk_resp resp;
  size_t resp_size = sizeof(resp);

  req.cmd = BPMP_CMD_CLK_GET_MAX_CLK_ID;
  req.id = 0;

  error_t err = bpmp_xfer(BPMP_MRQ_CLK, &req, sizeof(req), &resp, &resp_size);
  if(err)
    return err;
  uint32_t max_clk_id = resp.u32;
  for(int i = 1; i <= max_clk_id; i++) {
    req.cmd = BPMP_CMD_CLK_IS_ENABLED;
    req.id = i;
    resp_size = sizeof(resp);
    err = bpmp_xfer(BPMP_MRQ_CLK, &req, sizeof(req), &resp, &resp_size);
    if(err)
      continue;

    uint32_t enabled = resp.u32;

    req.cmd = BPMP_CMD_CLK_GET_ALL_INFO;
    req.id = i;
    resp_size = sizeof(resp);
    err = bpmp_xfer(BPMP_MRQ_CLK, &req, sizeof(req), &resp, &resp_size);
    if(err)
      return err;

    cli_printf(cli, "%4d %-40s %d\n",
               i, resp.all_info.name,
               enabled);
  }
  return 0;

}

CLI_CMD_DEF("clocks", cmd_clocks);


error_t
reset_peripheral(int id)
{
  struct bpmp_mrq_reset_req req = {BPMP_CMD_RESET_TOGGLE, id};
  return bpmp_xfer(BPMP_MRQ_RESET, &req, sizeof(req), NULL, NULL);
}

error_t
clk_enable(int id)
{
  struct bpmp_mrq_clk_req req = {BPMP_CMD_CLK_ENABLE, id};
  return bpmp_xfer(BPMP_MRQ_CLK, &req, sizeof(req), NULL, NULL);
}


static error_t
cmd_rst(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  return reset_peripheral(atoi(argv[1]));
}

CLI_CMD_DEF("rst", cmd_rst);



static error_t
cmd_clk_en(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  return clk_enable(atoi(argv[1]));
}

CLI_CMD_DEF("clk-en", cmd_clk_en);

