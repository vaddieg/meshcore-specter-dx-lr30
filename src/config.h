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
    uint8_t  loop_detect;      // 0-3 in bits [1:0] (off/minimal/moderate/strict).
                               // bits [3:2] = path.hash.mode (0=1B, 1=2B, 2=3B advert
                               // hash size). Packed here because the record is full
                               // (60 B) and this keeps the flash layout/CRC intact.
    uint8_t  txdelay_x10;      // 0-20
    uint8_t  direct_txdelay_x10;
    uint8_t  dutycycle;        // 1-100 %
    uint8_t  flood_max;        // 0-64
    uint8_t  rxgain;           // 0 = power-saving gain (default, matches official 'off'),
                               //     1 = boosted gain (more sensitive). Lives in the
                               //     record's former trailing pad, so size/CRC are
                               //     unchanged and existing devices default to 0.
};
static_assert(sizeof(NodeConfig) == 60, "NodeConfig must stay 60 B (flash words [2..16])");

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

// ─── Analog calibration (persisted in a third flash record) ──────────────────
// vrefint_mv: the internal VREFINT voltage (mV) that measure_vdda_mv() inverts
//   the ADC against to get the supply voltage. It is per-chip, so it's
//   runtime-calibratable: `battery.charged` (treats the current reading as a
//   fully-charged 4.2 V pack) or `set battery.vref <mV>`.
// temp_offset_c: an additive offset (whole °C, -12..+12) applied to the
//   internal temperature sensor, to zero it against a reference thermometer.
struct CalibConfig {
    uint16_t vrefint_mv;      // VREFINT in mV (default 1180)
    int16_t  temp_offset_c;   // temperature offset in °C (-12..+12, default 0)
};
extern CalibConfig cal;

// Analog calibration constants (shared by main.cpp measurement + cli.cpp).
// The battery-mod 1N5817 Schottky drop, added on top of VDDA to recover the
// pack voltage. Kept OUT of measure_vdda_mv() so that stays the clean supply
// reading used to scale the internal temperature sensor.
#define SHOTTKY_DROP_MV   264   // mV (VDDA → pack)
#define LIION_CHARGED_MV  4200  // mV (4.2 V), the `battery.charged` reference

// Compile-time default admin password, set via build flag:
//   -D ADMIN_PASSWORD="s3cret"
// Seeded into adm.admin_password at first boot (like NODE_LAT_I/NODE_LON_I); a
// CLI "set admin.password <pw>" — including clearing it — overrides it in flash.
#ifndef ADMIN_PASSWORD
#define ADMIN_PASSWORD ""
#endif

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
void set_rx_ring_log(bool on);   // `log start`/`log stop`: gate per-packet RX lines into the ring
bool rx_ring_log_enabled(void);

// ─── Serial state ───────────────────────────────────────────────────────────
// Always active (the PA9/PA10 auto-sleep feature was removed; see CHANGELOG).
bool serial_is_active(void);

// ─── hooks implemented in main.cpp, used by CLI handlers ───────────────────
void cli_reboot(void);
void cli_dfu(void);
void cli_send_advert_now(bool zero_hop);
void cli_set_rxgain(bool boosted);   // live setRxBoostedGainMode (no reboot)
void cli_set_time(uint32_t epoch);
uint32_t cli_now_epoch(void);
const char* cli_version_str(void);
uint16_t measure_vdda_mv(void);      // clean analog supply (VDDA) in mV (VREFINT)
uint16_t measure_battery_mv(void);   // pack voltage in mV = VDDA + Schottky drop
int16_t  measure_mcu_temp(void);     // MCU die temperature in °C (internal sensor)
