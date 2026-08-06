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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define BLE_ADV_PDU_MAX (2 + 6 + 31)
#define BLE_ADV_INTERVAL 100000 // µs, plus advDelay jitter

typedef struct {
  sx1280_slot_t slot;
  sx1280_t *chip;

  uint32_t tx_done;
  uint32_t tx_timeout;
  uint32_t cmd_errors;

  uint8_t sweep;      // Sweep whitening seeds, name carries the seed
  uint8_t sweep_seed;
  uint8_t sweep_cnt;

  char name[17];
} sx1280_ble_adv_t;

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

  const uint16_t irqmask = SX1280_IRQ_TX_DONE | SX1280_IRQ_RX_TX_TIMEOUT;
  const uint8_t dioirq[9] = {SX1280_SET_DIOIRQPARAMS,
                             irqmask >> 8, irqmask & 0xff,
                             irqmask >> 8, irqmask & 0xff, // DIO1
                             0, 0, 0, 0};
  return sx1280_cmd(s, dioirq, NULL, sizeof(dioirq));
}

static size_t
build_adv_pdu(uint8_t *p, const char *name)
{
  // Static random address, MSB (last on air) needs top two bits set
  static const uint8_t addr[6] = {0x28, 0x12, 0x80, 0x66, 0x66, 0xc6};

  const size_t namelen = strlen(name);

  p[0] = 0x42; // ADV_NONCONN_IND, TxAdd=1 (random address)
  p[1] = 6 + 3 + 2 + namelen;
  memcpy(p + 2, addr, 6);
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

  int irq = sx1280_wait_irq(s, 50000);
  if(irq < 0)
    return irq;
  if(irq & SX1280_IRQ_TX_DONE)
    a->tx_done++;
  else
    a->tx_timeout++;
  return 0;
}

static int64_t
ble_adv_execute(sx1280_slot_t *slot, sx1280_t *s, int64_t now)
{
  sx1280_ble_adv_t *a = (sx1280_ble_adv_t *)slot;
  error_t err = 0;

  if(sx1280_sched_set_mode(s, a)) {
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
sx1280_ble_adv_stats(sx1280_t *s, uint32_t *tx_done, uint32_t *tx_timeout,
                     uint32_t *cmd_errors)
{
  sx1280_ble_adv_t *a = sx1280_ble_adv_get(s);
  *tx_done = a->tx_done;
  *tx_timeout = a->tx_timeout;
  *cmd_errors = a->cmd_errors;
}
