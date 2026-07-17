// BLE Security Manager, peripheral role. Supports legacy Just Works pairing
// and LE Secure Connections (Just Works + Numeric Comparison), with bonding.
// Core v5.3 Vol 3 Part H.

#include "smp.h"
#include "smp_proto.h"
#include "smp_lesc.h"
#include "l2cap.h"
#include "ble_bond.h"

#include "net/pbuf.h"

#include <mios/eventlog.h>
#include <mios/service.h>

#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>

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

  // LE Secure Connections. Big-endian (spec/toolbox order).
  uint8_t sc;           // LESC negotiated
  uint8_t method;       // SMP_METHOD_*
  uint8_t user_ok;      // numeric comparison confirmed (or auto for Just Works)
  uint8_t ea_received;   // central's DHKey check has arrived
  uint8_t our_pub[64];   // our P-256 public key (X||Y)
  uint8_t our_priv[32];
  uint8_t peer_pub[64];  // central's public key
  uint8_t dhkey[32];
  uint8_t na[16], nb[16];// initiator / responder nonces
  uint8_t mackey[16];
  uint8_t ea[16];        // central's DHKey check value
  uint8_t iocap_init[3]; // initiator authreq||oob||iocap (for f6)
  uint8_t iocap_resp[3];

  ble_bond_t bond;      // assembled bond record (our LTK + peer identity)

  aes128_t aes;         // scratch (kept off the stack: 176 bytes expanded key)
} smp_t;

enum { SMP_METHOD_JUST_WORKS, SMP_METHOD_NUMERIC_COMPARISON };

enum {
  SMP_IDLE,
  SMP_W4_CONFIRM,  // legacy: sent Pairing Response, awaiting central's Confirm
  SMP_W4_RANDOM,   // legacy: sent our Confirm, awaiting central's Random
  SMP_W4_LTK,      // sent our Random, awaiting the controller LTK request
  SMP_W4_ENC,      // replied with STK, awaiting Encryption Change
  SMP_W4_KEYS,     // link encrypted, exchanging bonding keys
  SMP_ENCRYPTED,
  SMP_LESC_W4_PUBKEY, // LESC: awaiting central's public key
  SMP_LESC_W4_NRAND,  // LESC: sent our confirm, awaiting central's random
  SMP_LESC_W4_DHKCHK, // LESC: awaiting central's DHKey check (and user confirm)
};

// Reverse-copy n bytes: SMP transmits values little-endian, the crypto
// toolbox works big-endian.
static void
revcpy(uint8_t *dst, const uint8_t *src, int n)
{
  for(int i = 0; i < n; i++)
    dst[i] = src[n - 1 - i];
}


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

  s->bonding = !!(req->auth_req & SMP_AUTH_BONDING);
  // Use LE Secure Connections when the central supports it; the SC LTK is
  // derived, so no EncKey distribution. Ask the central for its identity key.
  s->sc = !!(req->auth_req & SMP_AUTH_SC);
  s->resp_kd = (s->bonding && !s->sc) ? (req->resp_key_dist & SMP_DIST_ENC) : 0;
  s->init_kd = s->bonding ? (req->init_key_dist & SMP_DIST_ID) : 0;

  // We are DisplayYesNo: the mios console shows the numeric-comparison value
  // and a CLI command confirms it. Request MITM so a capable central runs
  // Numeric Comparison (authenticated, level 4); it falls back to Just Works
  // automatically against a NoInputNoOutput central.
  smp_pairing_t rsp = {
    .code = SMP_PAIRING_RESPONSE,
    .io_capability = SMP_IO_DISPLAY_YESNO,
    .oob_data_flag = 0,
    .auth_req = (s->bonding ? SMP_AUTH_BONDING : 0) | SMP_AUTH_MITM |
                (s->sc ? SMP_AUTH_SC : 0),
    .max_enc_key_size = 16,
    .init_key_dist = s->init_kd,
    .resp_key_dist = s->resp_kd,
  };
  memcpy(s->pres, &rsp, 7);

  if(s->sc) {
    // IOcap (authreq||oob||iocap) of each side, for f6.
    s->iocap_init[0] = s->preq[3]; s->iocap_init[1] = s->preq[2];
    s->iocap_init[2] = s->preq[1];
    s->iocap_resp[0] = s->pres[3]; s->iocap_resp[1] = s->pres[2];
    s->iocap_resp[2] = s->pres[1];
    // Numeric Comparison only when both sides can display+confirm.
    const uint8_t io = req->io_capability;
    const int both_mitm = (req->auth_req & SMP_AUTH_MITM);
    s->method = (both_mitm &&
                 (io == SMP_IO_DISPLAY_YESNO || io == SMP_IO_KEYBOARD_DISPLAY))
                ? SMP_METHOD_NUMERIC_COMPARISON : SMP_METHOD_JUST_WORKS;
    s->state = SMP_LESC_W4_PUBKEY; // initiator sends its public key next
    smp_send(l2c, &rsp, 7);
    return;
  }

  // Legacy Just Works.
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

  s->bond.level = BLE_SEC_ENCRYPTED; // legacy Just Works is unauthenticated

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


// --- LE Secure Connections --------------------------------------------------
// A1/A2 for the toolbox: type byte then the 6 address bytes big-endian.
static void
lesc_addr(uint8_t out[7], uint8_t type, const uint8_t hci_addr[6])
{
  out[0] = type;
  revcpy(out + 1, hci_addr, 6);
}

static void
handle_lesc_public_key(l2cap_t *l2c, smp_t *s, const uint8_t *pdu, int len)
{
  if(s->state != SMP_LESC_W4_PUBKEY || len < 65)
    return smp_fail(l2c, SMP_ERR_UNSPECIFIED);

  // Wire public key is X||Y, each 32 bytes little-endian.
  revcpy(s->peer_pub, pdu + 1, 32);
  revcpy(s->peer_pub + 32, pdu + 33, 32);
  if(!smp_ecdh_valid(s->peer_pub))
    return smp_fail(l2c, SMP_ERR_UNSPECIFIED);

  if(!smp_ecdh_keygen(s->our_pub, s->our_priv) ||
     !smp_ecdh_shared(s->peer_pub, s->our_priv, s->dhkey))
    return smp_fail(l2c, SMP_ERR_UNSPECIFIED);

  uint8_t pk[65];
  pk[0] = SMP_PAIRING_PUBLIC_KEY;
  revcpy(pk + 1, s->our_pub, 32);
  revcpy(pk + 33, s->our_pub + 32, 32);
  smp_send(l2c, pk, sizeof(pk));

  // Responder commits first: Cb = f4(PKbx, PKax, Nb, 0).
  ble_rand(s->nb, sizeof(s->nb));
  uint8_t cb[16];
  smp_f4(s->our_pub, s->peer_pub, s->nb, 0, cb);

  uint8_t cfm[17];
  cfm[0] = SMP_PAIRING_CONFIRM;
  revcpy(cfm + 1, cb, 16);

  s->state = SMP_LESC_W4_NRAND;
  smp_send(l2c, cfm, sizeof(cfm));
}

// Compute LTK/MacKey, verify the central's DHKey check, answer with ours, and
// stage the bond. Runs once both the check has arrived and (for Numeric
// Comparison) the user has confirmed the value.
static void
lesc_try_finish(l2cap_t *l2c, smp_t *s)
{
  if(!s->ea_received || !s->user_ok)
    return;

  uint8_t a_init[7], a_resp[7];
  lesc_addr(a_init, l2c->l2c_peer_addr_type, l2c->l2c_peer_addr);
  lesc_addr(a_resp, l2c->l2c_our_addr_type, l2c->l2c_our_addr);

  uint8_t ltk[16];
  smp_f5(s->dhkey, s->na, s->nb, a_init, a_resp, s->mackey, ltk);

  static const uint8_t z16[16] = {0};
  uint8_t eacheck[16];
  smp_f6(s->mackey, s->na, s->nb, z16, s->iocap_init, a_init, a_resp, eacheck);
  if(memcmp(eacheck, s->ea, 16))
    return smp_fail(l2c, SMP_ERR_DHKEY_CHECK);

  uint8_t eb[16];
  smp_f6(s->mackey, s->nb, s->na, z16, s->iocap_resp, a_resp, a_init, eb);
  uint8_t pdu[17];
  pdu[0] = SMP_PAIRING_DHKEY_CHECK;
  revcpy(pdu + 1, eb, 16);
  smp_send(l2c, pdu, sizeof(pdu));

  // LESC LTK: identified by EDIV=0/Rand=0; stored big-endian? No -- the
  // controller wants it as the raw key; f5 already yields it in that order.
  // f5 yields the LTK big-endian; the controller (like the legacy STK path)
  // wants it little-endian.
  revcpy(s->bond.ltk, ltk, 16);
  s->bond.ediv = 0;
  memset(s->bond.rand, 0, 8);
  s->bond.sc = 1;
  s->bond.level = s->method == SMP_METHOD_NUMERIC_COMPARISON
                  ? BLE_SEC_AUTH_SC : BLE_SEC_ENCRYPTED;
  l2c->l2c_pending_sec_level = s->bond.level;

  s->state = SMP_W4_LTK;
}

static void
handle_lesc_random(l2cap_t *l2c, smp_t *s, const uint8_t *pdu, int len)
{
  if(s->state != SMP_LESC_W4_NRAND || len < 17)
    return smp_fail(l2c, SMP_ERR_UNSPECIFIED);

  revcpy(s->na, pdu + 1, 16);

  uint8_t rnd[17];
  rnd[0] = SMP_PAIRING_RANDOM;
  revcpy(rnd + 1, s->nb, 16);
  smp_send(l2c, rnd, sizeof(rnd));

  s->state = SMP_LESC_W4_DHKCHK;

  if(s->method == SMP_METHOD_NUMERIC_COMPARISON) {
    // g2(PKax, PKbx, Na, Nb); the low 6 digits are shown to the user.
    uint32_t v = smp_g2(s->peer_pub, s->our_pub, s->na, s->nb) % 1000000;
    s->user_ok = 0;
    evlog(LOG_NOTICE, "smp: numeric comparison %06u - check it matches the "
          "central, then run ble_confirm", (unsigned)v);
  } else {
    s->user_ok = 1; // Just Works: no user check
  }
}

static void
handle_lesc_dhkey_check(l2cap_t *l2c, smp_t *s, const uint8_t *pdu, int len)
{
  if(s->state != SMP_LESC_W4_DHKCHK || len < 17)
    return smp_fail(l2c, SMP_ERR_UNSPECIFIED);
  revcpy(s->ea, pdu + 1, 16);
  s->ea_received = 1;
  lesc_try_finish(l2c, s);
}

// CLI-driven confirm for Numeric Comparison.
error_t
smp_numeric_confirm(l2cap_t *l2c)
{
  smp_t *s = l2c->l2c_smp;
  if(s == NULL || s->state != SMP_LESC_W4_DHKCHK ||
     s->method != SMP_METHOD_NUMERIC_COMPARISON || s->user_ok)
    return ERR_BAD_STATE;
  s->user_ok = 1;
  lesc_try_finish(l2c, s);
  return 0;
}

void
smp_input(l2cap_t *l2c, pbuf_t *pb)
{
  // The largest SMP PDU (Pairing Public Key, 65 bytes) spans two ACL
  // fragments, so it arrives as a pbuf chain that exceeds one pbuf. Copy the
  // whole PDU into a contiguous buffer before parsing.
  uint8_t pdu[65];
  const int len = pb->pb_pktlen;

  if(len < 1 || len > (int)sizeof(pdu) || pbuf_read_at(pb, pdu, 0, len)) {
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
  case SMP_PAIRING_PUBLIC_KEY:
    handle_lesc_public_key(l2c, s, pdu, len);
    break;
  case SMP_PAIRING_CONFIRM:
    handle_pairing_confirm(l2c, s, pdu, len); // legacy only
    break;
  case SMP_PAIRING_RANDOM:
    if(s->sc)
      handle_lesc_random(l2c, s, pdu, len);
    else
      handle_pairing_random(l2c, s, pdu, len);
    break;
  case SMP_PAIRING_DHKEY_CHECK:
    handle_lesc_dhkey_check(l2c, s, pdu, len);
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
  // Ask for bonding + LE Secure Connections + MITM; the central's Pairing
  // Request drives the actual negotiation and we respond accordingly.
  const uint8_t pdu[2] = {
    SMP_SECURITY_REQUEST, SMP_AUTH_BONDING | SMP_AUTH_SC | SMP_AUTH_MITM,
  };
  smp_send(l2c, pdu, sizeof(pdu));
}


void
smp_ltk_request(l2cap_t *l2c, uint64_t random_number, uint16_t ediv)
{
  smp_t *s = l2c->l2c_smp;

  // Just-finished pairing: the initial encryption uses EDIV=0/Rand=0. Legacy
  // uses the STK; LE Secure Connections uses the derived LTK.
  if(s != NULL && s->state == SMP_W4_LTK &&
     random_number == 0 && ediv == 0) {
    s->state = SMP_W4_ENC;
    l2c->l2c_ltk_reply(l2c, s->sc ? s->bond.ltk : s->stk);
    return;
  }

  // Reconnect: find the stored bond (RAM lookup only, safe in this interrupt
  // context) and restore its security level for when encryption turns on.
  uint8_t rand[8];
  for(int i = 0; i < 8; i++)
    rand[i] = random_number >> (i * 8);

  ble_bond_t bond;
  int found = (ediv == 0 && random_number == 0)
    ? ble_bond_find_by_addr(l2c->l2c_peer_addr, l2c->l2c_peer_addr_type, &bond)
    : ble_bond_find_by_ediv(ediv, rand, &bond);

  if(found) {
    l2c->l2c_pending_sec_level = bond.level;
    l2c->l2c_ltk_reply(l2c, bond.ltk);
    return;
  }

  l2c->l2c_ltk_reply(l2c, NULL);
}


void
smp_encryption_changed(l2cap_t *l2c, int enabled)
{
  // Runs in the driver's interrupt context. Defer the real work (key PDUs,
  // flash) to the net thread via the l2cap task. The achieved level was
  // staged when the keys were derived (or, on reconnect, looked up from the
  // bond); apply it now.
  l2c->l2c_sec_level = enabled ? l2c->l2c_pending_sec_level : BLE_SEC_NONE;
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
