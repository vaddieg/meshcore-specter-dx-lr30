#include "acl.h"
#include <string.h>

static AclEntry entries[ACL_MAX];

AclEntry* acl_by_pub(const uint8_t* pub) {
    for (int i = 0; i < ACL_MAX; i++)
        if (entries[i].active && memcmp(entries[i].pub, pub, ACL_PUBKEY) == 0)
            return &entries[i];
    return 0;
}

AclEntry* acl_by_hash(uint8_t hash) {
    // 1-byte hashes: match active entries whose pub[0] == hash
    for (int i = 0; i < ACL_MAX; i++)
        if (entries[i].active && entries[i].pub[0] == hash)
            return &entries[i];
    return 0;
}

AclEntry* acl_add(const uint8_t* pub, uint8_t perms, const uint8_t* secret) {
    AclEntry* e = acl_by_pub(pub);
    if (!e) {
        for (int i = 0; i < ACL_MAX; i++) {
            if (!entries[i].active) {
                e = &entries[i];
                e->active = 1;
                e->last_ts = 0;
                e->last_activity_ms = 0;
                break;
            }
        }
        if (!e) return 0;   // table full
    }
    memcpy(e->pub, pub, ACL_PUBKEY);
    e->perms = perms;
    if (secret) memcpy(e->secret, secret, ACL_SECRET);
    return e;   // caller sets last_ts / last_activity_ms
}

void acl_remove_pub(const uint8_t* pub) {
    AclEntry* e = acl_by_pub(pub);
    if (e) e->active = 0;
}

void acl_clear(void) {
    for (int i = 0; i < ACL_MAX; i++) entries[i].active = 0;
}

int acl_count(void) {
    int c = 0;
    for (int i = 0; i < ACL_MAX; i++) if (entries[i].active) c++;
    return c;
}

const AclEntry* acl_at(int i) {
    return (i >= 0 && i < ACL_MAX) ? &entries[i] : 0;
}
