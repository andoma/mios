#pragma once

#include <stdint.h>

struct l2cap;
struct pbuf;

// BLE Security Manager (peripheral role). Just Works legacy pairing: enough to
// bring a link up encrypted. Bonding / LE Secure Connections come later.

// An SMP PDU arrived on l2cap channel 6.
void smp_input(struct l2cap *l2c, struct pbuf *pb);

// Ask the central to start pairing (SMP Security Request). Needed because some
// centrals (notably iOS) never initiate pairing on their own; the peripheral
// must request it.
void smp_request_security(struct l2cap *l2c);

// The controller (peripheral) received an encryption start and is asking the
// host for the key. random_number and ediv are 0 for legacy pairing's initial
// STK encryption.
void smp_ltk_request(struct l2cap *l2c, uint64_t random_number, uint16_t ediv);

// The link's encryption state changed (HCI Encryption Change). Runs in the
// driver's interrupt context; heavy work is deferred to smp_encrypted().
void smp_encryption_changed(struct l2cap *l2c, int enabled);

// Link is encrypted: distribute/collect bonding keys and persist. Called from
// the net thread (l2cap task), where flash and pbuf allocation are safe.
void smp_encrypted(struct l2cap *l2c);

// Tear down any pairing state (link disconnected).
void smp_fini(struct l2cap *l2c);

// Platform entropy for the pairing nonce. Weak default uses the C PRNG; a
// platform with a TRNG should override it.
void ble_rand(void *out, unsigned int len);
