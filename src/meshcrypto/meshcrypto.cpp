/*
 * MeshCore peer-data wire crypto (C++ wrapper over the rweather/Crypto lib).
 * Reproduces MeshCore src/Utils.cpp byte-for-byte: AES-128-ECB (no IV, zero-
 * padded) with a 2-byte truncated HMAC-SHA256 over the ciphertext.
 */
#include "meshcrypto.h"

#include <AES.h>
#include <SHA256.h>
#include <string.h>

// AES-128-ECB encrypt: each 16-byte block encrypted independently; the final
// partial block is zero-padded. Returns the ciphertext length (16-multiple).
static int aes_ecb_encrypt(const uint8_t* secret, uint8_t* dest,
                           const uint8_t* src, int src_len) {
    AES128 aes;
    aes.setKey(secret, MESH_CIPHER_KEY_SIZE);
    uint8_t* dp = dest;
    const uint8_t* sp = src;
    int rem = src_len;
    while (rem >= MESH_CIPHER_BLOCK_SIZE) {
        aes.encryptBlock(dp, sp);
        dp += MESH_CIPHER_BLOCK_SIZE; sp += MESH_CIPHER_BLOCK_SIZE; rem -= MESH_CIPHER_BLOCK_SIZE;
    }
    if (rem > 0) {                       // zero-pad the final partial block
        uint8_t tmp[MESH_CIPHER_BLOCK_SIZE];
        memset(tmp, 0, sizeof(tmp));
        memcpy(tmp, sp, rem);
        aes.encryptBlock(dp, tmp);
        dp += MESH_CIPHER_BLOCK_SIZE;
    }
    return (int)(dp - dest);
}

// AES-128-ECB decrypt: every 16-byte block (src_len is a 16-multiple).
static int aes_ecb_decrypt(const uint8_t* secret, uint8_t* dest,
                           const uint8_t* src, int src_len) {
    AES128 aes;
    aes.setKey(secret, MESH_CIPHER_KEY_SIZE);
    uint8_t* dp = dest;
    const uint8_t* sp = src;
    while ((int)(sp - src) < src_len) {
        aes.decryptBlock(dp, sp);
        dp += MESH_CIPHER_BLOCK_SIZE; sp += MESH_CIPHER_BLOCK_SIZE;
    }
    return (int)(dp - dest);
}

int mesh_encrypt_then_mac(const uint8_t* secret, uint8_t* dest,
                          const uint8_t* src, int src_len) {
    int enc_len = aes_ecb_encrypt(secret, dest + MESH_CIPHER_MAC_SIZE, src, src_len);
    {
        SHA256 sha;
        sha.resetHMAC(secret, MESH_PUB_KEY_SIZE);
        sha.update(dest + MESH_CIPHER_MAC_SIZE, enc_len);
        sha.finalizeHMAC(secret, MESH_PUB_KEY_SIZE, dest, MESH_CIPHER_MAC_SIZE);
    }
    return MESH_CIPHER_MAC_SIZE + enc_len;
}

int mesh_mac_then_decrypt(const uint8_t* secret, uint8_t* dest,
                          const uint8_t* src, int src_len) {
    if (src_len <= MESH_CIPHER_MAC_SIZE) return 0;         // no ciphertext
    int enc_len = src_len - MESH_CIPHER_MAC_SIZE;
    if (enc_len % MESH_CIPHER_BLOCK_SIZE != 0) return 0;   // not block-aligned

    uint8_t hmac[MESH_CIPHER_MAC_SIZE];
    {
        SHA256 sha;
        sha.resetHMAC(secret, MESH_PUB_KEY_SIZE);
        sha.update(src + MESH_CIPHER_MAC_SIZE, enc_len);
        sha.finalizeHMAC(secret, MESH_PUB_KEY_SIZE, hmac, MESH_CIPHER_MAC_SIZE);
    }
    if (memcmp(hmac, src, MESH_CIPHER_MAC_SIZE) != 0) return 0;  // MAC mismatch

    return aes_ecb_decrypt(secret, dest, src + MESH_CIPHER_MAC_SIZE, enc_len);
}
