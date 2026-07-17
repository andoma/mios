// BLE Security Manager, peripheral role. Just Works legacy pairing only: it
// brings a link up encrypted but does not yet bond (no key distribution) or
// do LE Secure Connections. Core v5.3 Vol 3 Part H.

#include "smp.h"
#include "smp_proto.h"
#include "l2cap.h"
#include "ble_bond.h"

#include "net/pbuf.h"

#include <mios/eventlog.h>
#include <mios/service.h>

#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include "lib/crypto/aes.h"

typedef struct smp {
  uint8_t state;
  uint8_t preq[7];      // Pairing Request  (from central)
  uint8_t pres[7];      // Pairing Response (ours)
  uint8_t mconfirm[16]; // central's confirm
  uint8_t srand[16];    // our random nonce
  uint8_t stk[16];      // short-term key handed to the controller

  // Bonding (key distribution phase).
  uint8_t bonding;      // negotiated bonding
  uint8_t init_kd;      // negotiated keys the central will send us
  uint8_t resp_kd;      // negotiated keys we will send
  uint8_t got_irk;      // received Identity Information
  uint8_t got_id_addr;  // received Identity Address Information

  ble_bond_t bond;      // assembled bond record (our LTK + peer identity)

  aes128_t aes;         // scratch (kept off the stack: 176 bytes expanded key)
} smp_t;

enum {
  SMP_IDLE,
  SMP_W4_CONFIRM,  // sent Pairing Response, awaiting central's Confirm
  SMP_W4_RANDOM,   // sent our Confirm, awaiting central's Random
  SMP_W4_LTK,      // sent our Random, awaiting the controller LTK request
  SMP_W4_ENC,      // replied with STK, awaiting Encryption Change
  SMP_W4_KEYS,     // link encrypted, exchanging bonding keys
  SMP_ENCRYPTED,
};


// --- crypto toolbox (validated against the spec sample data) ----------------

// AES-128 over little-endian buffers: the spec's e() takes big-endian key and
// data, while SMP values are little-endian on the wire, so reverse in and out.
// ctx is caller-provided scratch (the expanded key is 176 bytes, too large to
// sit on the net thread's stack under the frame-size limit).
static void
smp_e(aes128_t *ctx, const uint8_t k[16], uint8_t r[16])
{
  uint8_t kb[16], db[16];
  for(int i = 0; i < 16; i++) {
    kb[i] = k[15 - i];
    db[i] = r[15 - i];
  }
  aes128_init(ctx, kb);
  aes128_encrypt(ctx, db, db);
  for(int i = 0; i < 16; i++)
    r[i] = db[15 - i];
}

static void
xor16(uint8_t *o, const uint8_t *a, const uint8_t *b)
{
  for(int i = 0; i < 16; i++)
    o[i] = a[i] ^ b[i];
}

// c1(k, r, preq, pres, iat, ia, rat, ra): the legacy pairing confirm value.
static void
smp_c1(aes128_t *ctx, const uint8_t k[16], const uint8_t r[16],
       const uint8_t preq[7], const uint8_t pres[7],
       uint8_t iat, const uint8_t ia[6],
       uint8_t rat, const uint8_t ra[6], uint8_t res[16])
{
  uint8_t p1[16] = {0}, p2[16] = {0};
  p1[0] = iat;
  p1[1] = rat;
  memcpy(p1 + 2, preq, 7);
  memcpy(p1 + 9, pres, 7);
  memcpy(p2, ra, 6);
  memcpy(p2 + 6, ia, 6);

  xor16(res, r, p1);
  smp_e(ctx, k, res);
  xor16(res, res, p2);
  smp_e(ctx, k, res);
}

// s1(k, r1, r2): the legacy STK.
static void
smp_s1(aes128_t *ctx, const uint8_t k[16], const uint8_t r1[16],
       const uint8_t r2[16], uint8_t res[16])
{
  memcpy(res, r2, 8);
  memcpy(res + 8, r1, 8);
  smp_e(ctx, k, res);
}


// Weak entropy default; platforms with a TRNG override this.
__attribute__((weak)) void
ble_rand(void *out, unsigned int len)
{
  uint8_t *p = out;
  for(unsigned int i = 0; i < len; i++)
    p[i] = rand();
}


// --- PDU helpers ------------------------------------------------------------

static void
smp_send(l2cap_t *l2c, const void *data, size_t len)
{
  pbuf_t *pb = pbuf_make(8, 0); // headroom for l2cap + driver ACL headers
  if(pb == NULL)
    return;
  memcpy(pbuf_append(pb, len), data, len);
  l2cap_output(l2c, pb, L2CAP_CID_SMP);
}

static void
smp_fail(l2cap_t *l2c, uint8_t reason)
{
  const uint8_t pdu[2] = { SMP_PAIRING_FAILED, reason };
  smp_send(l2c, pdu, sizeof(pdu));
  if(l2c->l2c_smp)
    l2c->l2c_smp->state = SMP_IDLE;
  evlog(LOG_NOTICE, "smp: pairing failed (0x%x)", reason);
}

// The TK for Just Works is all zeros.
static const uint8_t smp_tk_justworks[16] = {0};


static void
handle_pairing_request(l2cap_t *l2c, smp_t *s, const uint8_t *pdu, int len)
{
  if(len < 7)
    return smp_fail(l2c, SMP_ERR_UNSPECIFIED);

  const smp_pairing_t *req = (const void *)pdu;
  if(req->max_enc_key_size < 7)
    return smp_fail(l2c, SMP_ERR_ENC_KEY_SIZE);

  memcpy(s->preq, pdu, 7);

  // Bond if the central asked to; distribute our LTK (EncKey) and collect the
  // central's identity key (IRK) so we can recognise it later. Just Works,
  // legacy (SC cleared).
  s->bonding = !!(req->auth_req & SMP_AUTH_BONDING);
  s->resp_kd = s->bonding ? (req->resp_key_dist & SMP_DIST_ENC) : 0;
  s->init_kd = s->bonding ? (req->init_key_dist & SMP_DIST_ID) : 0;

  smp_pairing_t rsp = {
    .code = SMP_PAIRING_RESPONSE,
    .io_capability = SMP_IO_NO_INPUT_NO_OUTPUT,
    .oob_data_flag = 0,
    .auth_req = s->bonding ? SMP_AUTH_BONDING : 0,
    .max_enc_key_size = 16,
    .init_key_dist = s->init_kd,
    .resp_key_dist = s->resp_kd,
  };
  memcpy(s->pres, &rsp, 7);

  ble_rand(s->srand, sizeof(s->srand));

  s->state = SMP_W4_CONFIRM;
  smp_send(l2c, &rsp, 7);
}


static void
handle_pairing_confirm(l2cap_t *l2c, smp_t *s, const uint8_t *pdu, int len)
{
  if(s->state != SMP_W4_CONFIRM || len < 17)
    return smp_fail(l2c, SMP_ERR_UNSPECIFIED);

  memcpy(s->mconfirm, pdu + 1, 16);

  // Our confirm: c1(TK, Srand, preq, pres, iat=peer, ia=peer, rat=us, ra=us).
  uint8_t sconfirm[17];
  sconfirm[0] = SMP_PAIRING_CONFIRM;
  smp_c1(&s->aes, smp_tk_justworks, s->srand, s->preq, s->pres,
         l2c->l2c_peer_addr_type, l2c->l2c_peer_addr,
         l2c->l2c_our_addr_type, l2c->l2c_our_addr, sconfirm + 1);

  s->state = SMP_W4_RANDOM;
  smp_send(l2c, sconfirm, sizeof(sconfirm));
}


static void
handle_pairing_random(l2cap_t *l2c, smp_t *s, const uint8_t *pdu, int len)
{
  if(s->state != SMP_W4_RANDOM || len < 17)
    return smp_fail(l2c, SMP_ERR_UNSPECIFIED);

  const uint8_t *mrand = pdu + 1;

  // Verify the central's confirm now that we have its random.
  uint8_t check[16];
  smp_c1(&s->aes, smp_tk_justworks, mrand, s->preq, s->pres,
         l2c->l2c_peer_addr_type, l2c->l2c_peer_addr,
         l2c->l2c_our_addr_type, l2c->l2c_our_addr, check);
  if(memcmp(check, s->mconfirm, 16))
    return smp_fail(l2c, SMP_ERR_CONFIRM_FAILED);

  // STK = s1(TK, Srand, Mrand); the central will start encryption with it.
  smp_s1(&s->aes, smp_tk_justworks, s->srand, mrand, s->stk);

  uint8_t rsp[17];
  rsp[0] = SMP_PAIRING_RANDOM;
  memcpy(rsp + 1, s->srand, 16);

  s->state = SMP_W4_LTK;
  smp_send(l2c, rsp, sizeof(rsp));
}


static void handle_identity_info(l2cap_t *l2c, smp_t *s,
                                 const uint8_t *pdu, int len);
static void handle_identity_addr_info(l2cap_t *l2c, smp_t *s,
                                      const uint8_t *pdu, int len);

void
smp_input(l2cap_t *l2c, pbuf_t *pb)
{
  const uint8_t *pdu = pbuf_cdata(pb, 0);
  const int len = pb->pb_pktlen;

  if(len < 1) {
    pbuf_free(pb);
    return;
  }

  const uint8_t code = pdu[0];

  if(l2c->l2c_ltk_reply == NULL) {
    // The controller has no link encryption (native link layer); refuse.
    smp_fail(l2c, SMP_ERR_PAIRING_NOTSUPP);
    pbuf_free(pb);
    return;
  }

  smp_t *s = l2c->l2c_smp;
  if(s == NULL) {
    s = xalloc(sizeof(smp_t), 0, MEM_MAY_FAIL);
    if(s == NULL) {
      smp_fail(l2c, SMP_ERR_UNSPECIFIED);
      pbuf_free(pb);
      return;
    }
    memset(s, 0, sizeof(smp_t));
    l2c->l2c_smp = s;
  }

  switch(code) {
  case SMP_PAIRING_REQUEST:
    handle_pairing_request(l2c, s, pdu, len);
    break;
  case SMP_PAIRING_CONFIRM:
    handle_pairing_confirm(l2c, s, pdu, len);
    break;
  case SMP_PAIRING_RANDOM:
    handle_pairing_random(l2c, s, pdu, len);
    break;
  case SMP_IDENTITY_INFO:
    handle_identity_info(l2c, s, pdu, len);
    break;
  case SMP_IDENTITY_ADDR_INFO:
    handle_identity_addr_info(l2c, s, pdu, len);
    break;
  case SMP_ENCRYPTION_INFO:
  case SMP_CENTRAL_IDENT:
  case SMP_SIGNING_INFO:
    // Keys the central may distribute that we do not store (this milestone).
    break;
  case SMP_PAIRING_FAILED:
    s->state = SMP_IDLE;
    evlog(LOG_NOTICE, "smp: central aborted pairing (0x%x)",
          len > 1 ? pdu[1] : 0);
    break;
  default:
    smp_fail(l2c, SMP_ERR_CMD_NOTSUPP);
    break;
  }

  pbuf_free(pb);
}


// --- bonding key distribution (all thread context) --------------------------

static void
smp_persist_bond(l2cap_t *l2c, smp_t *s)
{
  // Fall back to the connection address if the central sent no identity.
  if(!s->got_id_addr) {
    memcpy(s->bond.peer_addr, l2c->l2c_peer_addr, 6);
    s->bond.peer_addr_type = l2c->l2c_peer_addr_type;
  }
  ble_bond_add(&s->bond);
  s->state = SMP_ENCRYPTED;
  evlog(LOG_NOTICE, "smp: bonded");
}

// Persist once the central has sent every key we asked it for.
static void
smp_keys_maybe_done(l2cap_t *l2c, smp_t *s)
{
  if(s->init_kd & SMP_DIST_ID) {
    if(!s->got_irk || !s->got_id_addr)
      return;
  }
  smp_persist_bond(l2c, s);
}


void
smp_encrypted(l2cap_t *l2c)
{
  smp_t *s = l2c->l2c_smp;

  evlog(LOG_NOTICE, "smp: link encrypted");

  if(s == NULL || !s->bonding) {
    // Reconnect of an existing bond, or a non-bonding session: nothing to do.
    if(s)
      s->state = SMP_ENCRYPTED;
    return;
  }

  s->state = SMP_W4_KEYS;

  // Distribute our LTK: a fresh random key identified by EDIV/Rand.
  if(s->resp_kd & SMP_DIST_ENC) {
    ble_rand(s->bond.ltk, sizeof(s->bond.ltk));
    ble_rand(s->bond.rand, sizeof(s->bond.rand));
    uint16_t ediv;
    ble_rand(&ediv, sizeof(ediv));
    s->bond.ediv = ediv;

    uint8_t enc[17];
    enc[0] = SMP_ENCRYPTION_INFO;
    memcpy(enc + 1, s->bond.ltk, 16);
    smp_send(l2c, enc, sizeof(enc));

    uint8_t cid[11];
    cid[0] = SMP_CENTRAL_IDENT;
    cid[1] = ediv;
    cid[2] = ediv >> 8;
    memcpy(cid + 3, s->bond.rand, 8);
    smp_send(l2c, cid, sizeof(cid));
  }

  // If we asked the central for nothing, the bond is complete now.
  if(s->init_kd == 0)
    smp_persist_bond(l2c, s);
}


static void
handle_identity_info(l2cap_t *l2c, smp_t *s, const uint8_t *pdu, int len)
{
  if(s->state != SMP_W4_KEYS || len < 17)
    return;
  memcpy(s->bond.irk, pdu + 1, 16);
  s->got_irk = 1;
  smp_keys_maybe_done(l2c, s);
}

static void
handle_identity_addr_info(l2cap_t *l2c, smp_t *s, const uint8_t *pdu, int len)
{
  if(s->state != SMP_W4_KEYS || len < 8)
    return;
  s->bond.peer_addr_type = pdu[1];
  memcpy(s->bond.peer_addr, pdu + 2, 6);
  s->got_id_addr = 1;
  smp_keys_maybe_done(l2c, s);
}


void
smp_request_security(l2cap_t *l2c)
{
  if(l2c->l2c_ltk_reply == NULL)
    return; // controller cannot encrypt (native link layer)
  // Request bonding (no MITM: Just Works, legacy) so the central stores keys
  // and later reconnects encrypt without re-pairing.
  const uint8_t pdu[2] = { SMP_SECURITY_REQUEST, SMP_AUTH_BONDING };
  smp_send(l2c, pdu, sizeof(pdu));
}


void
smp_ltk_request(l2cap_t *l2c, uint64_t random_number, uint16_t ediv)
{
  smp_t *s = l2c->l2c_smp;

  // Legacy pairing's initial encryption uses EDIV=0 and Rand=0 with the STK.
  if(s != NULL && s->state == SMP_W4_LTK &&
     random_number == 0 && ediv == 0) {
    s->state = SMP_W4_ENC;
    l2c->l2c_ltk_reply(l2c, s->stk);
    return;
  }

  // Reconnect: the central selects a stored LTK by EDIV/Rand (RAM lookup only,
  // safe in this interrupt context).
  uint8_t rand[8];
  for(int i = 0; i < 8; i++)
    rand[i] = random_number >> (i * 8);

  ble_bond_t bond;
  if(ble_bond_find_by_ediv(ediv, rand, &bond)) {
    l2c->l2c_ltk_reply(l2c, bond.ltk);
    return;
  }

  l2c->l2c_ltk_reply(l2c, NULL);
}


void
smp_encryption_changed(l2cap_t *l2c, int enabled)
{
  // Runs in the driver's interrupt context. Defer the real work (key PDUs,
  // flash) to the net thread via the l2cap task. Everything we can pair today
  // is Just Works, so an encrypted link is exactly level ENCRYPTED (L2);
  // authenticated / LE Secure Connections will raise this when implemented.
  l2c->l2c_sec_level = enabled ? BLE_SEC_ENCRYPTED : BLE_SEC_NONE;
  if(enabled)
    net_task_raise(&l2c->l2c_task, L2CAP_SIGNAL_SMP);
  else if(l2c->l2c_smp)
    l2c->l2c_smp->state = SMP_IDLE;
}


void
smp_fini(l2cap_t *l2c)
{
  if(l2c->l2c_smp) {
    free(l2c->l2c_smp);
    l2c->l2c_smp = NULL;
  }
}
