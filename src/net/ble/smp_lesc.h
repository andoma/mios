#pragma once

#include <stdint.h>

// BLE LE Secure Connections crypto toolbox (Core v5.3 Vol 3 Part H 2.2.5+).
// All values are big-endian here (matching the spec sample data). The SMP
// state machine byte-reverses at the wire boundary, since SMP transmits them
// little-endian.

// f4: pairing confirm value = AES-CMAC_x(U || V || Z).
void smp_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16],
            uint8_t z, uint8_t out[16]);

// f5: derive MacKey and LTK from the DH key and the two nonces/addresses.
void smp_f5(const uint8_t dhkey[32], const uint8_t n1[16], const uint8_t n2[16],
            const uint8_t a1[7], const uint8_t a2[7],
            uint8_t mackey[16], uint8_t ltk[16]);

// f6: DH key check value = AES-CMAC_w(N1 || N2 || R || IOcap || A1 || A2).
void smp_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16],
            const uint8_t r[16], const uint8_t iocap[3],
            const uint8_t a1[7], const uint8_t a2[7], uint8_t out[16]);

// g2: numeric comparison value (return value mod 1000000 is the 6 digits).
uint32_t smp_g2(const uint8_t u[32], const uint8_t v[32],
                const uint8_t x[16], const uint8_t y[16]);

// ah: resolvable private address hash. r/out are 24-bit (3 bytes), big-endian.
void smp_ah(const uint8_t irk[16], const uint8_t r[3], uint8_t out[3]);

// P-256 ECDH (big-endian). Return 1 on success, 0 on failure.
int smp_ecdh_keygen(uint8_t pub[64], uint8_t priv[32]);
int smp_ecdh_shared(const uint8_t peer_pub[64], const uint8_t priv[32],
                    uint8_t dhkey[32]);
int smp_ecdh_valid(const uint8_t pub[64]);
