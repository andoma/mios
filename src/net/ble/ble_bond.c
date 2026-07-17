#include "ble_bond.h"

#include <mios/mios.h>
#include <mios/eventlog.h>

#include <string.h>

#include "irq.h"

#ifdef ENABLE_LITTLEFS
#include <mios/fs.h>
#define BOND_FILE "bonds.dat"
#endif

// Small fixed table: a handful of paired centrals is plenty for a personal
// device. The whole table is persisted as one file and mirrored in RAM.
#define BLE_BOND_MAX 4

static ble_bond_t bonds[BLE_BOND_MAX];


static void
ble_bond_save(void)
{
#ifdef ENABLE_LITTLEFS
  error_t err = fs_save(BOND_FILE, bonds, sizeof(bonds));
  if(err)
    evlog(LOG_WARNING, "bond: save failed (%d)", err);
#endif
}


#ifdef ENABLE_LITTLEFS
// Load the table from flash after the filesystem is mounted (board flash init
// is constructor 5100; this runs later, on the main thread, so blocking flash
// I/O is fine).
static void __attribute__((constructor(5300)))
ble_bond_load(void)
{
  size_t actual;
  error_t err = fs_load(BOND_FILE, bonds, sizeof(bonds), &actual);
  if(err || actual != sizeof(bonds)) {
    memset(bonds, 0, sizeof(bonds));
    return;
  }
  int n = 0;
  for(int i = 0; i < BLE_BOND_MAX; i++)
    n += bonds[i].valid;
  if(n)
    evlog(LOG_INFO, "bond: loaded %d bond%s", n, n == 1 ? "" : "s");
}
#endif


int
ble_bond_find_by_ediv(uint16_t ediv, const uint8_t rand[8], ble_bond_t *out)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  int found = 0;
  for(int i = 0; i < BLE_BOND_MAX; i++) {
    if(bonds[i].valid && bonds[i].ediv == ediv &&
       !memcmp(bonds[i].rand, rand, 8)) {
      *out = bonds[i];
      found = 1;
      break;
    }
  }
  irq_permit(q);
  return found;
}


int
ble_bond_add(const ble_bond_t *b)
{
  // Prefer the slot already holding this identity address; else a free slot;
  // else evict slot 0. The RAM update is brief and interrupt-safe; the flash
  // write happens afterwards, outside the lock.
  int slot = -1, freeslot = -1;
  int q = irq_forbid(IRQ_LEVEL_NET);
  for(int i = 0; i < BLE_BOND_MAX; i++) {
    if(bonds[i].valid && bonds[i].peer_addr_type == b->peer_addr_type &&
       !memcmp(bonds[i].peer_addr, b->peer_addr, 6)) {
      slot = i;
      break;
    }
    if(!bonds[i].valid && freeslot < 0)
      freeslot = i;
  }
  if(slot < 0)
    slot = freeslot >= 0 ? freeslot : 0;
  bonds[slot] = *b;
  bonds[slot].valid = 1;
  irq_permit(q);

  ble_bond_save();
  return 0;
}


int
ble_bond_count(void)
{
  int n = 0;
  for(int i = 0; i < BLE_BOND_MAX; i++)
    n += bonds[i].valid;
  return n;
}


void
ble_bond_clear(void)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  memset(bonds, 0, sizeof(bonds));
  irq_permit(q);
  ble_bond_save();
}


#include <mios/cli.h>

static error_t
cmd_bonds(cli_t *cli, int argc, char **argv)
{
  if(argc > 1 && !strcmp(argv[1], "clear")) {
    ble_bond_clear();
    cli_printf(cli, "Bonds cleared\n");
    return 0;
  }

  for(int i = 0; i < BLE_BOND_MAX; i++) {
    const ble_bond_t *b = &bonds[i];
    if(!b->valid)
      continue;
    cli_printf(cli, "%d: %02x:%02x:%02x:%02x:%02x:%02x (%s) ediv:0x%04x\n",
               i, b->peer_addr[5], b->peer_addr[4], b->peer_addr[3],
               b->peer_addr[2], b->peer_addr[1], b->peer_addr[0],
               b->peer_addr_type ? "random" : "public", b->ediv);
  }
  return 0;
}

CLI_CMD_DEF_EXT("ble_bonds", cmd_bonds, "[clear]", "List or clear BLE bonds");
