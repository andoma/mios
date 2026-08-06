// BLE advertising (non-connectable beacons) on SX1280
//
// Transmits ADV_NONCONN_IND on channels 37/38/39 as a radio scheduler
// slot. The whitening seed register is undocumented and loads
// bit-reversed relative to the BLE spec convention; sweep mode
// advertises seed candidates in the device name for empirical
// verification against a phone scanner.

#include "sx1280_i.h"
#include "sx1280_ble.h"
#include "sx1280_sched.h"

#include <mios/task.h>
#include <mios/eventlog.h>
#include <mios/type_macros.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define BLE_ADV_PDU_MAX (2 + 6 + 31)
#define BLE_ADV_INTERVAL 100000 // µs, plus advDelay jitter

#define BLE_SCAN_WINDOW 30000   // µs per channel visit

// Shared modem-config token: advertiser and scanner use the same
// radio setup
static const char ble_mode[] = "ble";

typedef struct {
  sx1280_slot_t slot;
  sx1280_t *chip;

  uint32_t tx_done;
  uint32_t tx_timeout;
  uint32_t cmd_errors;
  uint32_t rx_scan_req;
  uint32_t rx_conn_ind;
  uint32_t rx_other;
  uint32_t rx_crc_errors;
  uint32_t rx_early;       // DIO1 events completing <600µs into window
  uint32_t rx_misaddr;     // SCAN_REQ/CONNECT_IND not addressed to us
  uint32_t max_turnaround; // µs, TxDone -> SetRx accepted
  uint16_t early_irq;      // IRQ bits of most recent early event
  uint16_t early_us;       // and its latency from TxDone

  uint8_t sweep;      // Sweep whitening seeds, name carries the seed
  uint8_t sweep_seed;
  uint8_t sweep_cnt;

  char name[17];
} sx1280_ble_adv_t;

// Our static random device address (MSB last on air, top two bits 11)
static const uint8_t ble_our_addr[6] = {0x28, 0x12, 0x80, 0x66, 0x66, 0xc6};

static sx1280_ble_adv_t *g_adv;

// 2402, 2426, 2480 MHz
static const uint8_t adv_channels[3] = {37, 38, 39};
static const uint32_t adv_freq[3] = {2402000000u, 2426000000u, 2480000000u};

static uint8_t
bitrev7(uint8_t v)
{
  uint8_t r = 0;
  for(int i = 0; i < 7; i++)
    if(v & (1 << i))
      r |= 1 << (6 - i);
  return r;
}

static error_t
ble_radio_setup(sx1280_t *s)
{
  error_t err;

  static const uint8_t standby[2] = {SX1280_SET_STANDBY, SX1280_STDBY_RC};
  if((err = sx1280_cmd(s, standby, NULL, sizeof(standby))) != 0)
    return err;

  static const uint8_t pkttype[2] = {SX1280_SET_PACKETTYPE,
                                     SX1280_PACKET_TYPE_BLE};
  if((err = sx1280_cmd(s, pkttype, NULL, sizeof(pkttype))) != 0)
    return err;

  static const uint8_t modparams[4] = {SX1280_SET_MODULATIONPARAMS,
                                       SX1280_BLE_BR_1_000_BW_1_2,
                                       SX1280_BLE_MOD_IND_0_5,
                                       SX1280_BLE_BT_0_5};
  if((err = sx1280_cmd(s, modparams, NULL, sizeof(modparams))) != 0)
    return err;

  static const uint8_t pktparams[8] = {SX1280_SET_PACKETPARAMS,
                                       SX1280_BLE_PAYLOAD_MAX_37,
                                       SX1280_BLE_CRC_3B,
                                       0, // test payload, unused
                                       SX1280_BLE_WHITENING_ENABLE,
                                       0, 0, 0};
  if((err = sx1280_cmd(s, pktparams, NULL, sizeof(pktparams))) != 0)
    return err;

  static const uint8_t baseaddr[3] = {SX1280_SET_BUFFERBASEADDRESS, 0, 0};
  if((err = sx1280_cmd(s, baseaddr, NULL, sizeof(baseaddr))) != 0)
    return err;

  static const uint8_t aa[4] = {0x8e, 0x89, 0xbe, 0xd6};
  if((err = sx1280_write_reg(s, SX1280_REG_BLE_ACCESS_ADDR, aa, 4)) != 0)
    return err;

  static const uint8_t crcinit[3] = {0x55, 0x55, 0x55};
  if((err = sx1280_write_reg(s, SX1280_REG_CRC_INIT, crcinit, 3)) != 0)
    return err;

  // +10dBm (value = dBm + 18), 10µs ramp
  static const uint8_t txparams[3] = {SX1280_SET_TXPARAMS, 28, 0x80};
  if((err = sx1280_cmd(s, txparams, NULL, sizeof(txparams))) != 0)
    return err;

  // Park in FS between TX/RX: shortens the T_IFS turnaround
  static const uint8_t autofs[2] = {SX1280_SET_AUTOFS, 1};
  if((err = sx1280_cmd(s, autofs, NULL, sizeof(autofs))) != 0)
    return err;

  const uint16_t irqmask = SX1280_IRQ_TX_DONE | SX1280_IRQ_RX_DONE |
    SX1280_IRQ_CRC_ERROR | SX1280_IRQ_RX_TX_TIMEOUT;
  const uint8_t dioirq[9] = {SX1280_SET_DIOIRQPARAMS,
                             irqmask >> 8, irqmask & 0xff,
                             irqmask >> 8, irqmask & 0xff, // DIO1
                             0, 0, 0, 0};
  return sx1280_cmd(s, dioirq, NULL, sizeof(dioirq));
}

static size_t
build_adv_pdu(uint8_t *p, const char *name)
{
  const size_t namelen = strlen(name);

  p[0] = 0x40; // ADV_IND (connectable undirected), TxAdd=1 (random address)
  p[1] = 6 + 3 + 2 + namelen;
  memcpy(p + 2, ble_our_addr, 6);
  // Flags: LE general discoverable, no BR/EDR
  p[8] = 2;
  p[9] = 0x01;
  p[10] = 0x06;
  // Complete local name
  p[11] = namelen + 1;
  p[12] = 0x09;
  memcpy(p + 13, name, namelen);
  return 13 + namelen;
}

// seed_override >= 0 forces the whitening seed (sweep mode), otherwise
// it is derived from the channel
static error_t
ble_adv_tx(sx1280_ble_adv_t *a, int ch, const char *name, int seed_override)
{
  sx1280_t *s = a->chip;
  error_t err;

  const uint32_t f = SX1280_HZ_TO_FREQ(adv_freq[ch]);
  const uint8_t freq[4] = {SX1280_SET_RFFREQUENCY, f >> 16, f >> 8, f};
  if((err = sx1280_cmd(s, freq, NULL, sizeof(freq))) != 0)
    return err;

  // The whitening LFSR loads bit-reversed relative to the BLE spec's
  // (and nRF hardware's) convention: ch37 wants 0x53, not 0x65.
  // Verified empirically with a seed sweep against a phone scanner.
  uint8_t seed = bitrev7(0x40 | adv_channels[ch]);
  if(seed_override >= 0)
    seed = seed_override;
  if((err = sx1280_write_reg(s, SX1280_REG_WHITENING_SEED, &seed, 1)) != 0)
    return err;

  uint8_t buf[2 + BLE_ADV_PDU_MAX];
  buf[0] = SX1280_WRITE_BUFFER;
  buf[1] = 0;
  size_t pdulen = build_adv_pdu(buf + 2, name);
  if((err = sx1280_cmd(s, buf, NULL, 2 + pdulen)) != 0)
    return err;

  static const uint8_t clr[3] = {SX1280_CLR_IRQSTATUS, 0xff, 0xff};
  if((err = sx1280_cmd(s, clr, NULL, sizeof(clr))) != 0)
    return err;

  // 10ms timeout, packet is ~400µs
  static const uint8_t tx[4] = {SX1280_SET_TX, SX1280_TICK_SIZE_1_MS, 0, 10};
  if((err = sx1280_cmd(s, tx, NULL, sizeof(tx))) != 0)
    return err;

  // Spin on DIO1 for TxDone: the T_IFS response window opens 150µs
  // after our TX ends, so SetRx must be the FIRST thing we send.
  // IRQ status bookkeeping waits until the radio is already listening.
  const uint64_t deadline = clock_get() + 2000;
  while(!gpio_get_input(s->dio1)) {
    if(clock_get() > deadline) {
      a->tx_timeout++;
      return sx1280_wait_irq_poll(s, 0) < 0 ? ERR_TIMEOUT : 0;
    }
  }
  const uint64_t t0 = clock_get();

  // Ack TxDone first: SetRx issued during the chip's own TxDone->FS
  // (AutoFS) transition is silently lost
  int irq = sx1280_wait_irq_poll(s, 0);
  if(irq < 0)
    return irq;
  if(irq & SX1280_IRQ_TX_DONE)
    a->tx_done++;

  // Listen for SCAN_REQ / CONNECT_IND addressed to us: T_IFS (150µs)
  // + CONNECT_IND airtime (~352µs) + margin
  static const uint8_t rx[4] = {SX1280_SET_RX, SX1280_TICK_SIZE_1_MS, 0, 1};
  if((err = sx1280_cmd(s, rx, NULL, sizeof(rx))) != 0)
    return err;

  const uint32_t turnaround = clock_get() - t0;
  if(turnaround > a->max_turnaround)
    a->max_turnaround = turnaround;

  irq = sx1280_wait_irq_poll(s, 1500);
  if(irq < 0)
    return irq;

  // Directed responses (T_IFS-timed) complete within ~600µs of the
  // window opening; late completions are ambient traffic. Record the
  // early events to verify we are not deaf at the start of the window.
  const uint32_t rx_latency = clock_get() - t0;
  if(irq != 0 && rx_latency < 600) {
    a->rx_early++;
    a->early_irq = irq;
    a->early_us = rx_latency;
  }
  if(irq == 0 || (irq & SX1280_IRQ_RX_TX_TIMEOUT))
    return 0; // Nobody talked to us

  if(irq & SX1280_IRQ_CRC_ERROR) {
    a->rx_crc_errors++;
    return 0;
  }

  if(!(irq & SX1280_IRQ_RX_DONE))
    return 0;

  // GetRxBufferStatus reports the PDU payload length; the buffer
  // additionally holds the 2-byte header in front of it
  uint8_t bs[4] = {SX1280_GET_RXBUFFERSTATUS};
  if((err = sx1280_cmd(s, bs, bs, sizeof(bs))) != 0)
    return err;
  const uint8_t payloadlen = bs[2];
  const uint8_t offset = bs[3];

  if(payloadlen < 6 || payloadlen + 2 > BLE_ADV_PDU_MAX)
    return 0;

  uint8_t rbuf[3 + BLE_ADV_PDU_MAX];
  rbuf[0] = SX1280_READ_BUFFER;
  rbuf[1] = offset;
  rbuf[2] = 0;
  if((err = sx1280_cmd(s, rbuf, rbuf, 3 + 2 + payloadlen)) != 0)
    return err;

  const uint8_t *pdu = rbuf + 3;
  const uint8_t pdu_type = pdu[0] & 0xf;

  // SCAN_REQ: ScanA(6) + AdvA(6). CONNECT_IND: InitA(6) + AdvA(6) +
  // LLData(22). Both carry our address at offset 8.
  if(pdu_type == 0x3 && payloadlen >= 12 &&
     !memcmp(pdu + 8, ble_our_addr, 6)) {
    a->rx_scan_req++;
  } else if(pdu_type == 0x5 && payloadlen >= 34 &&
            !memcmp(pdu + 8, ble_our_addr, 6)) {
    a->rx_conn_ind++;
    const uint8_t *ll = pdu + 14;
    evlog(LOG_NOTICE, "%s: CONNECT_IND from %02x:%02x:%02x:%02x:%02x:%02x "
          "AA:%02x%02x%02x%02x interval:%d.%02dms timeout:%dms hop:%d",
          s->name,
          pdu[7], pdu[6], pdu[5], pdu[4], pdu[3], pdu[2],
          ll[3], ll[2], ll[1], ll[0],
          (ll[10] | ll[11] << 8) * 125 / 100,
          (ll[10] | ll[11] << 8) * 125 % 100,
          (ll[14] | ll[15] << 8) * 10,
          ll[21] & 0x1f);
  } else if((pdu_type == 0x3 || pdu_type == 0x5) && payloadlen >= 12) {
    // Directed PDU that failed our address check: log the first one
    a->rx_misaddr++;
    if(a->rx_misaddr == 1)
      evlog(LOG_NOTICE, "%s: type:%d for %02x:%02x:%02x:%02x:%02x:%02x "
            "(not us)", s->name, pdu_type,
            pdu[13], pdu[12], pdu[11], pdu[10], pdu[9], pdu[8]);
  } else {
    a->rx_other++;
  }
  return 0;
}

static int64_t
ble_adv_execute(sx1280_slot_t *slot, sx1280_t *s, int64_t now)
{
  sx1280_ble_adv_t *a = (sx1280_ble_adv_t *)slot;
  error_t err = 0;

  if(sx1280_sched_set_mode(s, ble_mode)) {
    err = ble_radio_setup(s);
    if(err) {
      a->cmd_errors++;
      sx1280_sched_recover(s);
      return now + 1000000;
    }
  }

  if(a->sweep) {
    // Channel 37 only: the correct seed is channel-dependent, so a
    // fixed candidate can only match one channel. The name carries
    // the candidate so a scanner shows which one decodes.
    char name[24];
    snprintf(name, sizeof(name), "mios-%02x", a->sweep_seed);
    err = ble_adv_tx(a, 0, name, a->sweep_seed);
    a->sweep_cnt++;
    if(a->sweep_cnt >= 3) { // ~300ms per candidate
      a->sweep_cnt = 0;
      a->sweep_seed = (a->sweep_seed + 1) & 0x7f;
    }
  } else {
    for(int ch = 0; ch < 3 && !err; ch++)
      err = ble_adv_tx(a, ch, a->name, -1);
  }

  if(err) {
    a->cmd_errors++;
    sx1280_sched_recover(s);
  }

  // advInterval + advDelay jitter
  return now + BLE_ADV_INTERVAL + (clock_get() & 0x1fff);
}

static sx1280_ble_adv_t *
sx1280_ble_adv_get(sx1280_t *s)
{
  if(g_adv == NULL) {
    g_adv = calloc(1, sizeof(sx1280_ble_adv_t));
    g_adv->chip = s;
    g_adv->slot.ss_execute = ble_adv_execute;
    g_adv->slot.ss_duration = 5000;
    g_adv->slot.ss_prio = 1;
    strcpy(g_adv->name, "mios");
  }
  return g_adv;
}

void
sx1280_ble_adv_start(sx1280_t *s, const char *name, int sweep)
{
  sx1280_ble_adv_t *a = sx1280_ble_adv_get(s);
  if(name != NULL)
    strlcpy(a->name, name, sizeof(a->name));
  a->sweep = sweep;
  sx1280_sched_submit(s, &a->slot, clock_get());
}

void
sx1280_ble_adv_stop(sx1280_t *s)
{
  if(g_adv != NULL)
    sx1280_sched_cancel(s, &g_adv->slot);
}

void
sx1280_ble_adv_report(sx1280_t *s, struct stream *st)
{
  const sx1280_ble_adv_t *a = sx1280_ble_adv_get(s);
  stprintf(st, "tx_done:%d tx_timeout:%d errors:%d\n",
           (int)a->tx_done, (int)a->tx_timeout, (int)a->cmd_errors);
  stprintf(st, "scan_req:%d conn_ind:%d misaddr:%d other:%d crc_errors:%d "
           "early:%d[irq:%04x @%dus] max_turnaround:%dus\n",
           (int)a->rx_scan_req, (int)a->rx_conn_ind, (int)a->rx_misaddr,
           (int)a->rx_other, (int)a->rx_crc_errors,
           (int)a->rx_early, a->early_irq, a->early_us,
           (int)a->max_turnaround);
}

