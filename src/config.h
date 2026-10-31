/*
 * SPECTER CLI — shared configuration & interface between main.cpp (radio)
 * and cli.cpp / prefs.cpp. See docs/cli.md for the command plan.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

// ─── Defaults (EU868, per docs/cli.md §2) ───────────────────────────────────
#define DEF_NAME        "SPECTER-XXXX"   // replaced by auto name / build flag
#define DEF_FREQ_KHZ    869618           // 869.618 MHz (MeshCore EU868 default)
#define DEF_BW_TENTH    625              // 62.5 kHz  (stored as kHz * 10)
#define DEF_SF          8
#define DEF_CR          8                // passed straight to RadioLib (tested)
#define DEF_TX_DBM      14               // safe under EU 27 dBm EIRP
#define DEF_FLOOD_ADV_H 47               // hours, 3-168 (MeshCore repeater default)
#define DEF_ZERO_ADV_M  60               // minutes, 0 (off) or 60-240
#define DEF_LOOP_DETECT 1                // 0 off, 1 minimal, 2 moderate, 3 strict
#define DEF_TXDELAY_X10 5                // 0.5 (flood retransmit delay factor)
#define DEF_DIRTX_X10   3                // 0.3 (direct, repeater)
#define DEF_DUTY_CYCLE  10               // % — EU 868 MHz aggregate limit
#define DEF_FLOOD_MAX   64
#define DEF_ADMIN_TIMEOUT 600            // s — idle admin session lifetime (0 = never)

// ─── Runtime node configuration (mutable, persisted in flash) ───────────────
struct NodeConfig {
    char    name[32];          // null-terminated; 23 B max with location
    int32_t lat_micro;         // degrees * 1e6  (0/0 = no location in advert)
    int32_t lon_micro;
    uint32_t freq_khz;         // MHz * 1000
    uint16_t bw_tenth;         // kHz * 10
    uint8_t  sf;               // 5-12
    uint8_t  cr;               // 5-8 (RadioLib passthrough)
    uint8_t  tx_dbm;           // 1-22
    uint8_t  flood_advert_h;   // 3-168 hours
    uint16_t zerohop_adv_min;  // 0 or 60-240 minutes
    uint8_t  repeat_en;        // 0/1
    uint8_t  loop_detect;      // 0-3
    uint8_t  txdelay_x10;      // 0-20
    uint8_t  direct_txdelay_x10;
    uint8_t  dutycycle;        // 1-100 %
    uint8_t  flood_max;        // 0-64
};

extern NodeConfig cfg;

// ─── Mesh-admin credentials (persisted in a separate flash record) ──────────
// Empty admin_password = mesh administration disabled. Empty guest_password =
// guest (read-only) access disabled. admin_timeout = idle session lifetime in
// seconds (0 = sessions never expire on their own; they still end on logout).
struct AdminConfig {
    char     admin_password[24];   // 23 chars max
    char     guest_password[24];   // 23 chars max
    uint16_t admin_timeout;        // seconds
};
extern AdminConfig adm;

// ─── prefs.cpp ──────────────────────────────────────────────────────────────
void prefs_load(void);    // apply build-flag defaults, then overlay flash
void prefs_save(void);    // persist cfg to flash page
void prefs_erase(void);   // clear flash page (factory reset)

// ─── cli.cpp ────────────────────────────────────────────────────────────────
void cli_begin(void);     // called in setup() after prefs_load()
void cli_poll(void);      // non-blocking line reader, call in loop()
int cli_execute(const char* cmd, char* outbuf, int outsize);  // run + capture reply (mesh-admin)

// Neighbor cache (8 most recent adverts), used by `neighbors`
void neighbor_add(const uint8_t* pubkey32, uint32_t ts, int8_t snr_x4);

// ─── Console log ring (main.cpp logs into it, `log` command reads it) ────────
void console_log(const char* line);
void console_clear(void);
int  console_recent(char* out_lines, int max_lines, int line_len); // returns count

// ─── hooks implemented in main.cpp, used by CLI handlers ───────────────────
void cli_reboot(void);
#ifdef DFU_BOOT0_PIN
void cli_dfu(void);
#endif
void cli_send_advert_now(bool zero_hop);
void cli_set_time(uint32_t epoch);
uint32_t cli_now_epoch(void);
const char* cli_version_str(void);
