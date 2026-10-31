/*
 * MeshCore peer-data wire crypto — byte-exact with the reference firmware so
 * the SPECTER interoperates with the official companion/repeater over the mesh.
 *
 *   - Per-peer key  = X25519(our Ed25519 seed, peer pub)   [ed25519_key_exchange]
 *   - Ciphertext    = AES-128-ECB (no IV), last block zero-padded to 16 bytes
 *   - MAC           = first 2 bytes of HMAC-SHA256(key=shared_secret,
 *                     msg=ciphertext), prepended to the ciphertext
 *   - AES key       = the first 16 bytes of the 32-byte shared secret
 *
 * Reference: MeshCore src/Utils.cpp (encrypt / encryptThenMAC / MACThenDecrypt)
 * and MeshCore.h (CIPHER_KEY_SIZE=16, CIPHER_MAC_SIZE=2, CIPHER_BLOCK_SIZE=16).
 */
#pragma once

#include <stdint.h>

#define MESH_PUB_KEY_SIZE      32
#define MESH_CIPHER_KEY_SIZE   16
#define MESH_CIPHER_BLOCK_SIZE 16
#define MESH_CIPHER_MAC_SIZE   2

#ifdef __cplusplus
extern "C" {
#endif

/* X25519 shared secret from Ed25519 keys (defined in key_exchange.c). */
void ed25519_key_exchange(unsigned char *shared_secret,
                          const unsigned char *public_key,
                          const unsigned char *private_key);

#ifdef __cplusplus
}
#endif

/* dest <- [ MAC(2) | AES-ECB-encrypt(src, zero-padded to a 16-multiple) ].
 * Returns MESH_CIPHER_MAC_SIZE + enc_len. `secret` is the 32-byte shared
 * secret. `dest` must have room for MESH_CIPHER_MAC_SIZE + 16*ceil(src_len/16). */
int mesh_encrypt_then_mac(const uint8_t *secret, uint8_t *dest,
                          const uint8_t *src, int src_len);

/* Verify the 2-byte MAC, then AES-ECB-decrypt the ciphertext into dest.
 * Returns the decrypted length (a multiple of 16; caller trims to the real
 * length), or 0 if the MAC does not match or the input is malformed. */
int mesh_mac_then_decrypt(const uint8_t *secret, uint8_t *dest,
                          const uint8_t *src, int src_len);
