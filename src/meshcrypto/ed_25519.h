/*
 * Minimal stand-in for the MeshCore `ed_25519.h` — only the key-exchange
 * prototype that key_exchange.c defines is needed by this project.
 */
#ifndef ED_25519_H
#define ED_25519_H

#ifdef __cplusplus
extern "C" {
#endif

/* X25519 shared secret derived from an Ed25519 keypair (Edwards→Montgomery
 * u-coordinate of `public_key`, clamped `private_key` seed as the scalar).
 * Byte-exact with the MeshCore reference, so it interoperates with the
 * official companion/repeater firmware. */
void ed25519_key_exchange(unsigned char *shared_secret,
                          const unsigned char *public_key,
                          const unsigned char *private_key);

#ifdef __cplusplus
}
#endif

#endif /* ED_25519_H */
