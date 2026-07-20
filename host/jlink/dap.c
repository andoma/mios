#include "dap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// SWD DP registers
#define DP_DPIDR    0x0 // RO
#define DP_ABORT    0x0 // WO
#define DP_CTRLSTAT 0x4 // bank 0
#define DP_SELECT   0x8 // WO
#define DP_RDBUFF   0xc // RO

#define ABORT_CLEAR_ALL 0x1e // STKCMPCLR|STKERRCLR|WDERRCLR|ORUNERRCLR

#define CTRLSTAT_ORUNDETECT   (1u << 0)
#define CTRLSTAT_STICKYORUN   (1u << 1)
#define CTRLSTAT_STICKYERR    (1u << 5)
#define CTRLSTAT_WDATAERR     (1u << 7)
#define CTRLSTAT_CDBGPWRUPREQ (1u << 28)
#define CTRLSTAT_CDBGPWRUPACK (1u << 29)
#define CTRLSTAT_CSYSPWRUPREQ (1u << 30)
#define CTRLSTAT_CSYSPWRUPACK (1u << 31)

#define CTRLSTAT_ERRORS \
  (CTRLSTAT_STICKYORUN | CTRLSTAT_STICKYERR | CTRLSTAT_WDATAERR)

// MEM-AP registers
#define AP_CSW 0x00
#define AP_TAR 0x04
#define AP_DRW 0x0c

#define CSW_SIZE_MASK    0x7
#define CSW_SIZE_32      0x2
#define CSW_ADDRINC_MASK 0x30
#define CSW_ADDRINC_ONE  0x10
#define CSW_DEVICEEN     (1u << 6)
#define CSW_HNONSEC      (1u << 30)

#define ACK_OK    1
#define ACK_WAIT  2
#define ACK_FAULT 4

#define XFER_BITS (46 + 8) // req 8 + trn 1 + ack 3 + trn/data+parity 34
                           // + up to 8 idle cycles for AP accesses
#define IDLE_BITS 8        // appended at end of every flush

#define MAX_RETRY 8

typedef struct pend {
  uint32_t *dst;  // NULL for writes or discarded reads
  size_t off;     // bit offset of first target-driven bit (ack)
  uint8_t rnw;
} pend_t;

struct dap {
  jlink_t *jl;
  size_t max_bits;

  uint8_t *dir;
  uint8_t *out;
  uint8_t *in;
  size_t nbits;

  pend_t *pend;
  size_t npend;
  size_t max_pend;

  uint32_t select;    // cached DP SELECT register
  int select_valid;

  uint32_t csw;       // cached MEM-AP CSW for word/autoinc access
  int csw_valid;

  char errmsg[256];
};

const char *
dap_errmsg(dap_t *d)
{
  return d->errmsg;
}

dap_t *
dap_create(jlink_t *jl)
{
  dap_t *d = calloc(1, sizeof(dap_t));
  d->jl = jl;
  d->max_bits = jlink_swd_max_bits(jl);
  const size_t bytes = (d->max_bits + 7) / 8;
  d->dir = malloc(bytes);
  d->out = malloc(bytes);
  d->in = malloc(bytes);
  d->max_pend = d->max_bits / XFER_BITS + 1;
  d->pend = malloc(d->max_pend * sizeof(pend_t));
  return d;
}

void
dap_destroy(dap_t *d)
{
  if(d == NULL)
    return;
  free(d->dir);
  free(d->out);
  free(d->in);
  free(d->pend);
  free(d);
}

static void
q_reset(dap_t *d)
{
  memset(d->dir, 0, (d->max_bits + 7) / 8);
  memset(d->out, 0, (d->max_bits + 7) / 8);
  d->nbits = 0;
  d->npend = 0;
}

static void
q_bit(dap_t *d, int output, int value)
{
  const size_t i = d->nbits >> 3;
  const int b = d->nbits & 7;
  if(output)
    d->dir[i] |= 1 << b;
  if(value)
    d->out[i] |= 1 << b;
  d->nbits++;
}

static void
q_bits(dap_t *d, int output, uint32_t value, int count)
{
  for(int i = 0; i < count; i++)
    q_bit(d, output, (value >> i) & 1);
}

static int
get_bit(const uint8_t *buf, size_t off)
{
  return (buf[off >> 3] >> (off & 7)) & 1;
}

// Returns 0 on success, -1 on hard error (errmsg set),
// 1 on retryable SWD failure (WAIT/FAULT/parity, errmsg set)
static int
dap_flush(dap_t *d)
{
  if(d->nbits == 0)
    return 0;

  q_bits(d, 1, 0, IDLE_BITS);

  if(jlink_swd_io(d->jl, d->dir, d->out, d->in, d->nbits)) {
    snprintf(d->errmsg, sizeof(d->errmsg), "%s", jlink_errmsg(d->jl));
    q_reset(d);
    return -1;
  }

  if(getenv("MIOS_JLINK_DEBUG") != NULL) {
    fprintf(stderr, "swd io %zd bits, %zd transfers\n", d->nbits, d->npend);
    for(size_t i = 0; i < (d->nbits + 7) / 8; i++)
      fprintf(stderr, "  %3zd: dir %02x out %02x in %02x\n",
              i, d->dir[i], d->out[i], d->in[i]);
  }

  int r = 0;
  for(size_t i = 0; i < d->npend; i++) {
    const pend_t *p = &d->pend[i];
    // The probe samples the target's response starting at the
    // turnaround cycle, so the ack appears at the first input bit
    const int ack =
      get_bit(d->in, p->off + 0) |
      (get_bit(d->in, p->off + 1) << 1) |
      (get_bit(d->in, p->off + 2) << 2);

    if(ack != ACK_OK) {
      snprintf(d->errmsg, sizeof(d->errmsg), "SWD %s (transfer %zd/%zd)",
               ack == ACK_WAIT ? "WAIT" : ack == ACK_FAULT ?
               "FAULT" : "protocol error", i, d->npend);
      d->select_valid = 0;
      r = 1;
      break;
    }

    if(p->rnw) {
      uint32_t v = 0;
      int par = 0;
      for(int b = 0; b < 32; b++) {
        const int bit = get_bit(d->in, p->off + 3 + b);
        v |= (uint32_t)bit << b;
        par ^= bit;
      }
      if(par != get_bit(d->in, p->off + 35)) {
        snprintf(d->errmsg, sizeof(d->errmsg), "SWD read parity error");
        d->select_valid = 0;
        r = 1;
        break;
      }
      if(p->dst != NULL)
        *p->dst = v;
    }
  }
  q_reset(d);
  return r;
}

static int
q_xfer(dap_t *d, int apndp, int rnw, int reg, uint32_t wdata, uint32_t *rdata)
{
  if(d->nbits + XFER_BITS + IDLE_BITS > d->max_bits ||
     d->npend == d->max_pend) {
    const int r = dap_flush(d);
    if(r)
      return r;
  }

  const int a2 = (reg >> 2) & 1;
  const int a3 = (reg >> 3) & 1;
  const int parity = apndp ^ rnw ^ a2 ^ a3;
  const uint8_t req = 0x81 | (apndp << 1) | (rnw << 2) |
    (a2 << 3) | (a3 << 4) | (parity << 5);

  q_bits(d, 1, req, 8);

  pend_t *p = &d->pend[d->npend++];
  p->rnw = rnw;
  p->dst = rdata;
  p->off = d->nbits;

  if(rnw) {
    // trn + ack + data + parity + trn, all target-driven
    q_bits(d, 0, 0, 1 + 3 + 32 + 1 + 1);
  } else {
    q_bits(d, 0, 0, 1 + 3 + 1); // trn + ack + trn
    q_bits(d, 1, wdata, 32);
    int par = 0;
    for(int b = 0; b < 32; b++)
      par ^= (wdata >> b) & 1;
    q_bit(d, 1, par);
  }

  // Idle cycles after AP accesses reduce the chance of WAIT responses
  if(apndp)
    q_bits(d, 1, 0, 8);
  return 0;
}

// Make sure DP SELECT matches the AP/bank we are about to access
static int
q_select(dap_t *d, int apndp, int apsel, int reg)
{
  uint32_t sel;

  if(apndp)
    sel = ((uint32_t)apsel << 24) | (reg & 0xf0);
  else
    sel = d->select_valid ? (d->select & 0xffffff00) : 0; // DPBANKSEL = 0

  if(d->select_valid && d->select == sel)
    return 0;

  const int r = q_xfer(d, 0, 0, DP_SELECT, sel, NULL);
  if(r == 0) {
    d->select = sel;
    d->select_valid = 1;
  }
  return r;
}

// Clear sticky error flags after a failed batch
static int
dap_clear_errors(dap_t *d)
{
  q_reset(d);
  if(q_xfer(d, 0, 0, DP_ABORT, ABORT_CLEAR_ALL, NULL))
    return -1;
  return dap_flush(d) ? -1 : 0;
}

static int
dp_op(dap_t *d, int rnw, int reg, uint32_t wdata, uint32_t *rdata)
{
  for(int attempt = 0; attempt < MAX_RETRY; attempt++) {
    int r = q_select(d, 0, 0, reg);
    if(r == 0)
      r = q_xfer(d, 0, rnw, reg, wdata, rdata);
    if(r == 0)
      r = dap_flush(d);
    if(r <= 0)
      return r;
    dap_clear_errors(d);
    usleep(1000);
  }
  return -1;
}

int
dap_dp_read(dap_t *d, int reg, uint32_t *value)
{
  return dp_op(d, 1, reg, 0, value);
}

int
dap_dp_write(dap_t *d, int reg, uint32_t value)
{
  return dp_op(d, 0, reg, value, NULL);
}

static int
ap_op(dap_t *d, int apsel, int rnw, int reg, uint32_t wdata, uint32_t *rdata)
{
  for(int attempt = 0; attempt < MAX_RETRY; attempt++) {
    int r = q_select(d, 1, apsel, reg);
    if(r == 0)
      r = q_xfer(d, 1, rnw, reg, wdata, NULL);
    if(r == 0 && rnw)
      r = q_xfer(d, 0, 1, DP_RDBUFF, 0, rdata); // collect posted AP read
    if(r == 0)
      r = dap_flush(d);
    if(r <= 0)
      return r;
    dap_clear_errors(d);
    usleep(1000);
  }
  return -1;
}

int
dap_ap_read(dap_t *d, int apsel, int reg, uint32_t *value)
{
  return ap_op(d, apsel, 1, reg, 0, value);
}

int
dap_ap_write(dap_t *d, int apsel, int reg, uint32_t value)
{
  return ap_op(d, apsel, 0, reg, value, NULL);
}

// Line reset, JTAG-to-SWD switch sequence, line reset, idle.
// Bits transmitted LSB-first per byte.
static const uint8_t seq_jtag_to_swd[] = {
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x9e, 0xe7,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x00,
};

// Dormant-to-SWD: selection alert sequence + SWD activation code (0x1a)
// + line reset. Newer DPs (e.g. nRF54L) power up in dormant state.
static const uint8_t seq_dormant_to_swd[] = {
  0xff,
  0x92, 0xf3, 0x09, 0x62, 0x95, 0x2d, 0x85, 0x86,
  0xe9, 0xaf, 0xdd, 0xe3, 0xa2, 0x0e, 0xbc, 0x19,
  0xa0, 0xf1, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x00,
};

static int
try_connect(dap_t *d, const uint8_t *seq, size_t len, uint32_t *dpidr)
{
  q_reset(d);
  d->select_valid = 0;
  d->csw_valid = 0;

  for(size_t i = 0; i < len * 8; i++)
    q_bit(d, 1, (seq[i / 8] >> (i & 7)) & 1);
  if(dap_flush(d))
    return -1;

  return q_xfer(d, 0, 1, DP_DPIDR, 0, dpidr) || dap_flush(d);
}

int
dap_connect(dap_t *d, uint32_t *dpidr)
{
  if(try_connect(d, seq_jtag_to_swd, sizeof(seq_jtag_to_swd), dpidr) &&
     try_connect(d, seq_dormant_to_swd, sizeof(seq_dormant_to_swd), dpidr)) {
    snprintf(d->errmsg, sizeof(d->errmsg),
             "No response to SWD DPIDR read (target not connected?)");
    return -1;
  }

  if(dap_dp_write(d, DP_ABORT, ABORT_CLEAR_ALL))
    return -1;
  if(dap_dp_write(d, DP_SELECT, 0))
    return -1;
  d->select = 0;
  d->select_valid = 1;

  // Power up debug domain, enable overrun detection (required for
  // our batched transfers which always clock the full data phase)
  const uint32_t req =
    CTRLSTAT_CDBGPWRUPREQ | CTRLSTAT_CSYSPWRUPREQ | CTRLSTAT_ORUNDETECT;
  if(dap_dp_write(d, DP_CTRLSTAT, req))
    return -1;

  for(int i = 0; ; i++) {
    uint32_t st;
    if(dap_dp_read(d, DP_CTRLSTAT, &st))
      return -1;
    if((st & (CTRLSTAT_CDBGPWRUPACK | CTRLSTAT_CSYSPWRUPACK)) ==
       (CTRLSTAT_CDBGPWRUPACK | CTRLSTAT_CSYSPWRUPACK))
      break;
    if(i == 100) {
      snprintf(d->errmsg, sizeof(d->errmsg),
               "Debug domain power-up timeout (CTRL/STAT=0x%08x)", st);
      return -1;
    }
    usleep(1000);
  }
  return 0;
}

int
dap_mem_init(dap_t *d, int *device_en)
{
  uint32_t csw;
  if(dap_ap_read(d, 0, AP_CSW, &csw))
    return -1;

  if(device_en != NULL)
    *device_en = !!(csw & CSW_DEVICEEN);

  // 32-bit accesses, auto-increment, secure (clear HNONSEC)
  csw &= ~(CSW_SIZE_MASK | CSW_ADDRINC_MASK | CSW_HNONSEC);
  csw |= CSW_SIZE_32 | CSW_ADDRINC_ONE;

  if(dap_ap_write(d, 0, AP_CSW, csw))
    return -1;
  d->csw = csw;
  d->csw_valid = 1;
  return 0;
}

int
dap_mem_read32(dap_t *d, uint32_t addr, uint32_t *value)
{
  if(dap_ap_write(d, 0, AP_TAR, addr))
    return -1;
  return dap_ap_read(d, 0, AP_DRW, value);
}

int
dap_mem_write32(dap_t *d, uint32_t addr, uint32_t value)
{
  if(dap_ap_write(d, 0, AP_TAR, addr))
    return -1;
  return dap_ap_write(d, 0, AP_DRW, value);
}

// Check accumulated sticky errors after a bulk transfer
static int
dap_check_errors(dap_t *d, const char *what)
{
  uint32_t st;
  if(dap_dp_read(d, DP_CTRLSTAT, &st))
    return -1;
  if(st & CTRLSTAT_ERRORS) {
    snprintf(d->errmsg, sizeof(d->errmsg),
             "%s failed, CTRL/STAT=0x%08x", what, st);
    dap_clear_errors(d);
    return -1;
  }
  return 0;
}

// 512 bytes per chunk: fits in one probe buffer and never crosses
// the 1kB TAR auto-increment boundary
#define MEM_CHUNK_WORDS 128

int
dap_mem_write_block(dap_t *d, uint32_t addr, const void *data, size_t len)
{
  const uint8_t *u8 = data;

  if((addr | len) & 3) {
    snprintf(d->errmsg, sizeof(d->errmsg), "Unaligned block write");
    return -1;
  }

  while(len) {
    size_t chunk = 512 - (addr & 511);
    if(chunk > len)
      chunk = len;
    const size_t nwords = chunk / 4;

    int attempt;
    for(attempt = 0; attempt < MAX_RETRY; attempt++) {
      int r = q_select(d, 1, 0, AP_TAR);
      if(r == 0)
        r = q_xfer(d, 1, 0, AP_TAR, addr, NULL);
      for(size_t i = 0; r == 0 && i < nwords; i++) {
        uint32_t w;
        memcpy(&w, u8 + i * 4, 4);
        r = q_xfer(d, 1, 0, AP_DRW, w, NULL);
      }
      if(r == 0)
        r = dap_flush(d);
      if(r < 0)
        return -1;
      if(r == 0)
        break;
      dap_clear_errors(d); // retryable, redo whole chunk
    }
    if(attempt == MAX_RETRY)
      return -1;

    addr += chunk;
    u8 += chunk;
    len -= chunk;
  }
  return dap_check_errors(d, "Block write");
}

int
dap_mem_read_block(dap_t *d, uint32_t addr, void *data, size_t len)
{
  uint8_t *u8 = data;

  if((addr | len) & 3) {
    snprintf(d->errmsg, sizeof(d->errmsg), "Unaligned block read");
    return -1;
  }

  while(len) {
    size_t chunk = 512 - (addr & 511);
    if(chunk > len)
      chunk = len;
    const size_t nwords = chunk / 4;
    uint32_t words[MEM_CHUNK_WORDS];

    int attempt;
    for(attempt = 0; attempt < MAX_RETRY; attempt++) {
      int r = q_select(d, 1, 0, AP_TAR);
      if(r == 0)
        r = q_xfer(d, 1, 0, AP_TAR, addr, NULL);
      // AP reads are posted: transfer i returns the result of
      // transfer i-1; the first is discarded and RDBUFF returns the last
      for(size_t i = 0; r == 0 && i < nwords; i++)
        r = q_xfer(d, 1, 1, AP_DRW, 0, i ? &words[i - 1] : NULL);
      if(r == 0)
        r = q_xfer(d, 0, 1, DP_RDBUFF, 0, &words[nwords - 1]);
      if(r == 0)
        r = dap_flush(d);
      if(r < 0)
        return -1;
      if(r == 0)
        break;
      dap_clear_errors(d);
    }
    if(attempt == MAX_RETRY)
      return -1;

    memcpy(u8, words, chunk);
    addr += chunk;
    u8 += chunk;
    len -= chunk;
  }
  return dap_check_errors(d, "Block read");
}
