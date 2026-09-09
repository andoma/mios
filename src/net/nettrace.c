#include <mios/nettrace.h>

#include <mios/cli.h>
#include <mios/timer.h>
#include <mios/task.h>
#include <mios/mios.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "irq.h"
#include "util/crc32.h"

static nettrace_ent_t nettrace_buf[NETTRACE_ENTRIES];
static uint16_t nettrace_used;
static uint8_t nettrace_armed;

// Two ids, because a request/response pair is the smallest thing worth
// following. Nothing matches until armed.
static uint32_t nettrace_ids[2];

__attribute__((weak)) const char *
nettrace_tag_name(uint8_t tag)
{
  (void)tag;
  return NULL;
}

// Called from the net thread and from driver threads, so the append has
// to be atomic against both.
static void
nettrace_put(uint8_t tag, uint32_t id, uint32_t key, size_t len)
{
  const int q = irq_forbid(IRQ_LEVEL_NET);
  if(nettrace_armed && nettrace_used < NETTRACE_ENTRIES) {
    nettrace_ent_t *e = &nettrace_buf[nettrace_used++];
    e->nt_key = key;
    e->nt_ts = clock_get();
    e->nt_id = id;
    e->nt_tag = tag;
    e->nt_len = len > 255 ? 255 : len;
  }
  irq_permit(q);
}

void
nettrace_pkt(uint8_t tag, uint32_t id, const void *data, size_t len)
{
  if(!nettrace_armed)
    return;
  if(id != nettrace_ids[0] && id != nettrace_ids[1])
    return;
  nettrace_put(tag, id, ~crc32(0, data, len), len);
}

void
nettrace_mark(uint8_t tag, uint32_t key)
{
  if(!nettrace_armed)
    return;
  nettrace_put(tag, 0, key, 0);
}

static const char *
tagstr(uint8_t tag)
{
  switch(tag) {
  case NETTRACE_RX:   return "rx";
  case NETTRACE_TX:   return "tx";
  case NETTRACE_MARK: return "mark";
  default:            return nettrace_tag_name(tag);
  }
}

static error_t
cmd_nettrace(cli_t *cli, int argc, char **argv)
{
  if(argc > 1) {
    nettrace_armed = 0;
    nettrace_used = 0;
    if(!strcmp(argv[1], "off")) {
      cli_printf(cli, "disarmed\n");
      return 0;
    }
    nettrace_ids[0] = atoix(argv[1]);
    nettrace_ids[1] = argc > 2 ? atoix(argv[2]) : nettrace_ids[0];
    nettrace_armed = 1;
    cli_printf(cli, "armed 0x%x 0x%x, %d slots\n",
               (unsigned)nettrace_ids[0], (unsigned)nettrace_ids[1],
               NETTRACE_ENTRIES);
    return 0;
  }

  // Absolute microseconds, so entries from two boards can be lined up on
  // a mark. The delta column is only meaningful within one dump.
  cli_printf(cli, "%d/%d entries%s\n", nettrace_used, NETTRACE_ENTRIES,
             nettrace_used == NETTRACE_ENTRIES ? " (full)" : "");
  uint32_t prev = nettrace_used ? nettrace_buf[0].nt_ts : 0;
  for(int i = 0; i < nettrace_used; i++) {
    const nettrace_ent_t *e = &nettrace_buf[i];
    const char *t = tagstr(e->nt_tag);
    if(t == NULL) {
      cli_printf(cli, "%u %u %d 0x%03x %08x %d\n", e->nt_ts, e->nt_ts - prev,
                 e->nt_tag, e->nt_id, e->nt_key, e->nt_len);
    } else {
      cli_printf(cli, "%u %u %s 0x%03x %08x %d\n", e->nt_ts, e->nt_ts - prev,
                 t, e->nt_id, e->nt_key, e->nt_len);
    }
    prev = e->nt_ts;
  }
  return 0;
}

CLI_CMD_DEF_EXT("nettrace", cmd_nettrace, "[off | <id> [id]]",
                "Dump, or arm, the packet path trace");
