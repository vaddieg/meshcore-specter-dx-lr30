/*
 * SPECTER mesh-admin session table (in-memory, not persisted).
 *
 * A "session" is created by an authenticated ANON_REQ login and identified by
 * the client's (ephemeral) Ed25519 public key. The per-session X25519 shared
 * secret is stored so subsequent TXT_MSG commands can be decrypted. A reboot
 * clears all sessions (a feature, not a bug). Idle sessions are expired by
 * main.cpp using adm.admin_timeout.
 */
#pragma once

#include <stdint.h>

#define ACL_MAX     4
#define ACL_PUBKEY  32
#define ACL_SECRET  32

// permission levels (lower bits, matching MeshCore PERM_ACL_*)
#define PERM_GUEST     0
#define PERM_READ      1
#define PERM_READWRITE 2
#define PERM_ADMIN     3

typedef struct {
    uint8_t  pub[ACL_PUBKEY];      // session identity = client (ephemeral) pub
    uint8_t  secret[ACL_SECRET];   // per-session X25519 shared secret
    uint8_t  perms;                // PERM_*
    uint8_t  active;
    uint32_t last_activity_ms;     // our clock (idle timeout)
    uint32_t last_ts;              // their clock (login replay guard)
} AclEntry;

AclEntry*      acl_by_hash(uint8_t hash);            // active entry with pub[0]==hash
AclEntry*      acl_by_pub(const uint8_t* pub);       // active entry by full pubkey
AclEntry*      acl_add(const uint8_t* pub, uint8_t perms, const uint8_t* secret);
void           acl_remove_pub(const uint8_t* pub);
void           acl_clear(void);
int            acl_count(void);
const AclEntry* acl_at(int i);                       // for `acl` listing (may be inactive)
