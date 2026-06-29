#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <mios/mios.h>
#include <mios/task.h>

#include "nrf54l_reg.h"
#include "nrf54l_radio.h"

// HFXO: the radio needs the crystal oscillator running for an accurate
// 2.4 GHz carrier.
#define CLOCK_BASE             0x5010e000
#define CLOCK_TASKS_XOSTART    (CLOCK_BASE + 0x000)
#define CLOCK_EVENTS_XOSTARTED (CLOCK_BASE + 0x100)

// nRF54L RADIO (the modern Nordic radio IP: register map differs a lot from
// the nRF52 one - events at 0x200+, config block at 0xE00+).
#define RADIO_BASE             0x5008a000
#define RADIO_TASKS_TXEN       (RADIO_BASE + 0x000)
#define RADIO_TASKS_DISABLE    (RADIO_BASE + 0x010)
#define RADIO_EVENTS_END       (RADIO_BASE + 0x218)
#define RADIO_EVENTS_DISABLED  (RADIO_BASE + 0x220)
#define RADIO_SHORTS           (RADIO_BASE + 0x400)
#define RADIO_MODE             (RADIO_BASE + 0x500)
#define RADIO_DATAWHITE        (RADIO_BASE + 0x540)
#define RADIO_FREQUENCY        (RADIO_BASE + 0x708)
#define RADIO_TXPOWER          (RADIO_BASE + 0x710)
#define RADIO_PCNF0            (RADIO_BASE + 0xe20)
#define RADIO_PCNF1            (RADIO_BASE + 0xe28)
#define RADIO_BASE0            (RADIO_BASE + 0xe2c)
#define RADIO_PREFIX0          (RADIO_BASE + 0xe34)
#define RADIO_TXADDRESS        (RADIO_BASE + 0xe3c)
#define RADIO_RXADDRESSES      (RADIO_BASE + 0xe40)
#define RADIO_CRCCNF           (RADIO_BASE + 0xe44)
#define RADIO_CRCPOLY          (RADIO_BASE + 0xe48)
#define RADIO_CRCINIT          (RADIO_BASE + 0xe4c)
#define RADIO_PACKETPTR        (RADIO_BASE + 0xed0)

#define RADIO_MODE_BLE_1MBIT   3
#define RADIO_SHORT_READY_START (1 << 0)
#define RADIO_TXPOWER_0DBM     0x18

// BLE advertising access address and the channel<->RF-frequency mapping.
#define BLE_ADV_ACCESS_ADDR    0x8e89bed6
#define BLE_ADV_CRCINIT        0x555555

// FICR factory device address (same layout as nRF52: two words).
#define FICR_DEVICEADDR0       0x00ffc3a4
#define FICR_DEVICEADDR1       0x00ffc3a8

// The three BLE advertising channels and their MHz offset above 2400.
static const uint8_t adv_ch[3]   = {37, 38, 39};
static const uint8_t adv_freq[3] = {2, 26, 80};

// PDU header + 6 byte AdvA + up to 31 bytes AdvData.
static uint8_t adv_pkt[2 + 6 + 31] __attribute__((aligned(4)));
static uint8_t ble_addr[6];


static void
radio_config_ble(void)
{
  reg_wr(RADIO_MODE, RADIO_MODE_BLE_1MBIT);

  // 8-bit length field, 1-byte S0 (the PDU header byte), no S1.
  reg_wr(RADIO_PCNF0, (8 << 0) | (1 << 8) | (0 << 16));

  // MAXLEN=255, base address length 4 (BALEN=3), whitening enabled.
  reg_wr(RADIO_PCNF1, (255 << 0) | (3 << 16) | (1 << 25));

  // 3-byte CRC, computed over the PDU (skip the access address).
  reg_wr(RADIO_CRCCNF, (1 << 8) | (3 << 0));
  reg_wr(RADIO_CRCPOLY, 0x65b);
  reg_wr(RADIO_CRCINIT, BLE_ADV_CRCINIT);

  // Logical address 0 = PREFIX0[7:0] : BASE0[31:8] = the access address.
  reg_wr(RADIO_BASE0, BLE_ADV_ACCESS_ADDR << 8);
  reg_wr(RADIO_PREFIX0, BLE_ADV_ACCESS_ADDR >> 24);
  reg_wr(RADIO_TXADDRESS, 0);
  reg_wr(RADIO_RXADDRESSES, 1);

  reg_wr(RADIO_TXPOWER, RADIO_TXPOWER_0DBM);
  reg_wr(RADIO_SHORTS, RADIO_SHORT_READY_START);
}


static void
tx_one(int i)
{
  reg_wr(RADIO_FREQUENCY, adv_freq[i]);
  // Whitening: keep the BLE polynomial (DATAWHITE.POLY reset = 0x89) and set
  // the per-channel IV = channel index with bit 6 preset, per the BLE spec.
  reg_wr(RADIO_DATAWHITE, (0x89 << 16) | (0x40 | adv_ch[i]));
  reg_wr(RADIO_PACKETPTR, (intptr_t)adv_pkt);

  reg_wr(RADIO_EVENTS_END, 0);
  reg_wr(RADIO_EVENTS_DISABLED, 0);

  reg_wr(RADIO_TASKS_TXEN, 1);            // ramp up; READY_START shortcut -> TX
  while(!reg_rd(RADIO_EVENTS_END)) {}     // wait for the packet to go out

  reg_wr(RADIO_TASKS_DISABLE, 1);         // back to DISABLED before next channel
  while(!reg_rd(RADIO_EVENTS_DISABLED)) {}
}


static void *
adv_thread(void *arg)
{
  // Start the HFXO and wait for it to be running.
  reg_wr(CLOCK_EVENTS_XOSTARTED, 0);
  reg_wr(CLOCK_TASKS_XOSTART, 1);
  while(!reg_rd(CLOCK_EVENTS_XOSTARTED)) {}

  radio_config_ble();

  while(1) {
    for(int i = 0; i < 3; i++)
      tx_one(i);
    usleep(100000); // advertising interval ~100 ms
  }
  return NULL;
}


static void
build_adv_pkt(const char *name)
{
  // Random static address from the factory device address (top two bits set).
  uint32_t a0 = reg_rd(FICR_DEVICEADDR0);
  uint32_t a1 = reg_rd(FICR_DEVICEADDR1);
  ble_addr[0] = a0;
  ble_addr[1] = a0 >> 8;
  ble_addr[2] = a0 >> 16;
  ble_addr[3] = a0 >> 24;
  ble_addr[4] = a1;
  ble_addr[5] = (a1 >> 8) | 0xc0;

  size_t namelen = strlen(name);
  if(namelen > 29) // 31 byte AdvData budget minus the 2-byte name AD header
    namelen = 29;

  uint8_t *p = adv_pkt;
  p[0] = 0x40;               // PDU: ADV_IND, TxAdd=1 (random address)
  p[1] = 6 + 2 + namelen;    // payload length
  memcpy(p + 2, ble_addr, 6);
  p[8] = namelen + 1;
  p[9] = 0x09;               // AD type: Complete Local Name
  memcpy(p + 10, name, namelen);
}


void
nrf54l_radio_ble_adv_init(const char *name)
{
  build_adv_pkt(name);
  thread_create(adv_thread, NULL, 1024, "bleadv", 0, 0);
  printf("BLE: advertising as '%s' (%02x:%02x:%02x:%02x:%02x:%02x)\n", name,
         ble_addr[5], ble_addr[4], ble_addr[3],
         ble_addr[2], ble_addr[1], ble_addr[0]);
}
