/*
 * SPECTER, Native MeshCore Flood Repeater
 * Hardware: STM32F103C8T6 + SX1262 (DX-LR30 module)
 * Version: 1.2.0
 *
 * Pin mapping (confirmed from BoardInfo + sx1262_driver.h):
 *   SPI1 : SCK=PA5  MISO=PA6  MOSI=PA7
 *   SX1262: NSS=PA4  BUSY=PA2  NRST=PA3
 *   RF switch: TXEN=PA0  RXEN=PA1
 *   LED  : PB11 (active HIGH)
 *   UART1: TX=PA9  RX=PA10  → CH340 USB (requires ENABLE_HWSERIAL1 flag)
 *
 * Radio settings: runtime-configurable via CLI (docs/cli.md),
 * EU868 defaults: 869.618 MHz  SF8  BW 62.5 kHz  SyncWord 0x12
 *
 * CLI: USB-serial MeshCore command subset (ver, name, lat/lon, radio,
 * advert intervals, neighbors, stats, ...). See docs/cli.md.
 */

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <SHA256.h>
#include <Ed25519.h>
#include <IWatchdog.h>
#include "config.h"

// ─── Version ─────────────────────────────────────────────────────────────────
#define FW_VERSION "1.14.0"

// ─── Pin definitions ─────────────────────────────────────────────────────────
#define PIN_NSS   PA4
#define PIN_DIO1  PC15   // Verified connected to PC15 on this HW revision (shared with LSE OSC32_OUT)
#define PIN_NRST  PA3
#define PIN_BUSY  PA2
#define PIN_TXEN  PA0
#define PIN_RXEN  PA1
#define PIN_LED   PB11
#define LED_ON    HIGH   // DX-LR30: LED anode on PB11 (active HIGH)
#define LED_OFF   LOW

// ─── Radio settings ──────────────────────────────────────────────────────────
#define LORA_PREAMBLE 8
#define LORA_SYNC   0x12         // LoRa private network

// ─── MeshCore protocol constants ─────────────────────────────────────────────
#define ROUTE_TRANSPORT_FLOOD  0x00
#define ROUTE_FLOOD            0x01
#define ROUTE_DIRECT           0x02
#define ROUTE_TRANSPORT_DIRECT 0x03
#define PAYLOAD_ADVERT         0x04
#define PAYLOAD_TYPE_TRACE     0x09
#define MAX_HOPS               64
#define MAX_PKT                256

// ─── Timing ──────────────────────────────────────────────────────────────────
#define FIRST_ADVERT_MS        5000UL                   // 5 s after boot
#define DISCOVERY_BURST        5                        // zero-hop adverts
#define DISCOVERY_INTERVAL_MS  1000UL
#define DUTY_SILENCE_MAX_MS    30000UL

// ─── Node identity ────────────────────────────────────────────────────────────
// Default: auto-generated name "SPECTER-XXXX" (in prefs defaults) + key
// derived from STM32 UID.
//
// For a fixed identity (e.g. to control the path-hash byte), set in platformio.ini:
//   -D NODE_NAME_STR=\"SPECTER-1811\"
//   -D FIXED_PRIVKEY_HEX=\"<64 hex chars>\"   ← 32-byte Ed25519 seed
//
// The path hash is always pubkey[0], so pick a seed whose public key starts
// with the desired byte. Use tools/gen_identity.py to search for one.
static uint8_t own_privkey[32];
uint8_t own_pubkey[32];
static uint8_t own_hash;     // 1-byte path hash = own_pubkey[0]

// ── Compile-time hex → bytes helper (constexpr-friendly for a macro) ─────────
static void hex2bin(const char* hex, uint8_t* out, int len) {
    for (int i = 0; i < len; i++) {
        uint8_t hi = hex[2*i];
        uint8_t lo = hex[2*i+1];
        hi = (hi >= 'a') ? hi-'a'+10 : (hi >= 'A') ? hi-'A'+10 : hi-'0';
        lo = (lo >= 'a') ? lo-'a'+10 : (lo >= 'A') ? lo-'A'+10 : lo-'0';
        out[i] = (hi << 4) | lo;
    }
}

static void identity_init(void) {
    // If auto-generated placeholder name, replace with UID-based one
    if (strcmp(cfg.name, DEF_NAME) == 0) {
        const uint32_t uid_lo = *(volatile uint32_t*)0x1FFFF7F0U;
        snprintf(cfg.name, sizeof(cfg.name), "SPECTER-%04X",
                 (unsigned)(uid_lo & 0xFFFF));
    }
#ifdef FIXED_PRIVKEY_HEX
    // ── Fixed identity: use pre-generated keypair with known path hash ───────
    hex2bin(FIXED_PRIVKEY_HEX, own_privkey, 32);
    Ed25519::derivePublicKey(own_pubkey, own_privkey);
#else
    // ── Auto identity: derive deterministic key from STM32 UID ───────────────
    const uint32_t uid[3] = {
        *(volatile uint32_t*)0x1FFFF7E8U,
        *(volatile uint32_t*)0x1FFFF7ECU,
        *(volatile uint32_t*)0x1FFFF7F0U,
    };
    SHA256 sha;
    sha.reset();
    const char* domain = "SPECTER-MESHCORE-KEY-V1";
    sha.update((const uint8_t*)domain, strlen(domain));
    sha.update((const uint8_t*)uid, 12);
    sha.finalize(own_privkey, 32);
    Ed25519::derivePublicKey(own_pubkey, own_privkey);
#endif
    own_hash = own_pubkey[0];
}

// ─── Dedup cache (circular, 128 slots × 8-byte SHA256 fingerprint) ───────────
// Fingerprint = first 8 bytes of SHA256(ptype ‖ payload), matching the
// reference repeater.py key: packet_fingerprint(ptype_byte + payload).
#define DEDUP_SIZE 128
#define DEDUP_FP   8
static uint8_t  dedup_buf[DEDUP_SIZE][DEDUP_FP];
static uint8_t  dedup_head = 0;

static bool dedup_seen(const uint8_t* payload, uint8_t ptype, uint8_t plen) {
    // Compute SHA256(ptype || payload), keep first DEDUP_FP bytes
    uint8_t fp[DEDUP_FP];
    {
        SHA256 sha;
        sha.reset();
        sha.update(&ptype, 1);
        sha.update(payload, plen);
        uint8_t full[32];
        sha.finalize(full, 32);
        memcpy(fp, full, DEDUP_FP);
    }
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (memcmp(dedup_buf[i], fp, DEDUP_FP) == 0) return true;
    }
    memcpy(dedup_buf[dedup_head], fp, DEDUP_FP);
    dedup_head = (dedup_head + 1) % DEDUP_SIZE;
    return false;
}

// ─── RF switch helpers ───────────────────────────────────────────────────────
static void rf_rx(void)   { digitalWrite(PIN_TXEN, LOW);  digitalWrite(PIN_RXEN, HIGH); }
static void rf_tx(void)   { digitalWrite(PIN_TXEN, HIGH); digitalWrite(PIN_RXEN, LOW);  }
static void rf_idle(void) { digitalWrite(PIN_TXEN, LOW);  digitalWrite(PIN_RXEN, LOW); }

// LED patterns (active HIGH on PB11)
//   1 blink        = packet received
//   2 fast blinks  = relay transmitted (connectivity confirmed)
//   solid ON       = ADVERT transmitting
static void led_blink(uint8_t n, uint16_t ms) {
    for (uint8_t i = 0; i < n; i++) {
        digitalWrite(PIN_LED, LED_ON);
        delay(ms);
        digitalWrite(PIN_LED, LED_OFF);
        if (i < n - 1) delay(ms);
    }
}

// ─── Compile-time Unix timestamp ────────────────────────────────────────────
// Converts __DATE__ "Mmm DD YYYY" + __TIME__ "HH:MM:SS" → Unix epoch seconds.
// Used as timestamp base so ADVERTs carry valid Unix time (companions may
// reject packets where |now - ts| > 3600 s as replay-protection).
// Re-sync at runtime with the CLI `time <epoch>` command.
static uint32_t build_unix_ts(void) {
    const char* d = __DATE__;   // "May 31 2026"
    const char* t = __TIME__;   // "14:32:33"
    // Month
    static const char* mn = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int month = 1;
    for (int m = 0; m < 12; m++) {
        if (d[0]==mn[m*3] && d[1]==mn[m*3+1] && d[2]==mn[m*3+2]) { month=m+1; break; }
    }
    int day  = (d[4]==' ' ? 0 : d[4]-'0')*10 + (d[5]-'0');
    int year = (d[7]-'0')*1000+(d[8]-'0')*100+(d[9]-'0')*10+(d[10]-'0');
    int hour = (t[0]-'0')*10+(t[1]-'0');
    int min  = (t[3]-'0')*10+(t[4]-'0');
    int sec  = (t[6]-'0')*10+(t[7]-'0');
    // Days from 1970 to Jan 1 of build year
    int y = year - 1970;
    uint32_t days = (uint32_t)y * 365UL + (uint32_t)((y + 1) / 4);
    // Days in each month up to build month
    const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int m = 1; m < month; m++) {
        days += md[m-1];
        if (m == 2 && (year % 4 == 0)) days++;
    }
    days += (uint32_t)(day - 1);
    return days * 86400UL + (uint32_t)hour*3600UL + (uint32_t)min*60UL + (uint32_t)sec;
}
static uint32_t ts_base = 0;   // set in setup(); millis() added at runtime

// ─── RadioLib object ─────────────────────────────────────────────────────────
SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY, SPI);

static uint32_t radio_airtime_ms = 200;   // estimated, set in radio_init()
static bool     radio_ok         = false; // radio_init() has succeeded
static uint32_t last_radio_retry = 0;     // degraded-mode retry timer

// ─── Token-bucket RF duty cycle (matches the MeshCore Dispatcher) ────────────
// A budget of airtime (ms) refills continuously at the duty rate over a 1-hour
// window. Bursts up to the bucket are allowed; each TX consumes its measured
// airtime and is gated until enough budget is available. This is far smoother
// than a fixed per-packet silence: idle time tops the bucket up, so a burst
// after idle is fine, and the average airtime over the window stays ≤ duty.
#define DUTY_WINDOW_MS 3600000UL          // 1 h window (MeshCore standard)
static uint32_t tx_budget_ms       = 0;   // available airtime budget (ms)
static uint32_t last_budget_update = 0;   // last refill timestamp (ms)

// ─── DIO1 interrupt flag ─────────────────────────────────────────────────────
// Set by the DIO1 ISR when the radio raises an interrupt (RxDone).
// The main loop polls this flag instead of continuously reading getIrqFlags().
static volatile bool rx_flag = false;

static void on_dio1(void) {
    rx_flag = true;
}

static bool radio_init(void) {
    SPI.begin();
    float freq_mhz = cfg.freq_khz / 1000.0f;
    float bw_khz   = cfg.bw_tenth / 10.0f;
    int state = radio.begin(freq_mhz, bw_khz, cfg.sf, cfg.cr,
                            LORA_SYNC, cfg.tx_dbm, LORA_PREAMBLE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial1.print("FAILED: "); Serial1.println(state);
        return false;
    }
    radio.setRfSwitchPins(PIN_RXEN, PIN_TXEN);
    radio.setCRC(2);   // CRC-16 on
    radio.setDio1Action(on_dio1);   // DIO1 (PC15) fires on RxDone → sets rx_flag

    // Estimate worst-case on-air time for duty-cycle enforcement:
    // symbol time = 2^sf / bw; ~160 symbols covers preamble + ~50 B payload
    float sym_ms = (float)(1UL << (cfg.sf & 0x0F)) / bw_khz;
    radio_airtime_ms = (uint32_t)(sym_ms * 160.0f) + 50;
    if (radio_airtime_ms < 50)  radio_airtime_ms = 50;
    if (radio_airtime_ms > 2000) radio_airtime_ms = 2000;
    return true;
}

// ─── Build ADVERT packet (MeshCore v1 protocol with Ed25519 signature) ───────
// ⚠️  CRITICAL: MeshCore companions validate Ed25519 signatures on ADVERTs.
//     Unsigned or incorrectly signed packets are silently discarded.
//     This was the root cause of "0 repeaters" during early development.
//     See docs/development-notes.md#ed25519-signature-requirement for details.
static int build_advert(uint8_t* buf) {
    int i = 0;
    // Header: ROUTE_FLOOD | (PAYLOAD_ADVERT << 2) = 0x11
    buf[i++] = (uint8_t)(ROUTE_FLOOD | (PAYLOAD_ADVERT << 2));
    // Path length: 0 hops, 1-byte hashes
    buf[i++] = 0x00;

    // --- Payload ---
    // pubkey (32 bytes)
    memcpy(buf + i, own_pubkey, 32); i += 32;

    // timestamp (4 bytes LE): compile-time Unix base + uptime seconds
    uint32_t ts = ts_base + millis() / 1000UL;
    memcpy(buf + i, &ts, 4); i += 4;

    // signature placeholder (filled in after appdata is known)
    int sig_offset = i;
    memset(buf + i, 0, 64); i += 64;

    // appdata: flags [+ lat/lon] [+ name]
    int appdata_offset = i;
    bool has_loc = (cfg.lat_micro != 0 || cfg.lon_micro != 0);
    if (has_loc) {
        buf[i++] = 0x92;  // FLAG_IS_REPEATER(0x02) | FLAG_HAS_LOCATION(0x10) | FLAG_HAS_NAME(0x80)
        int32_t lat = cfg.lat_micro;
        int32_t lon = cfg.lon_micro;
        memcpy(buf + i, &lat, 4); i += 4;
        memcpy(buf + i, &lon, 4); i += 4;
    } else {
        buf[i++] = 0x82;  // FLAG_IS_REPEATER(0x02) | FLAG_HAS_NAME(0x80)
    }
    // Name (advert limit: 23 B with location, 31 without)
    uint8_t nlen = (uint8_t)strlen(cfg.name);
    uint8_t maxn = has_loc ? 23 : 31;
    if (nlen > maxn) nlen = maxn;
    memcpy(buf + i, cfg.name, nlen); i += nlen;
    int appdata_len = i - appdata_offset;

    // Sign: message = pubkey(32) + timestamp(4) + appdata
    uint8_t msg[32 + 4 + 96];  // appdata: 1 flags + 8 GPS + 31 name = 40 bytes max
    int mlen = 0;
    memcpy(msg + mlen, own_pubkey, 32);           mlen += 32;
    memcpy(msg + mlen, buf + 2 + 32, 4);          mlen += 4;   // timestamp from buf
    memcpy(msg + mlen, buf + appdata_offset, appdata_len); mlen += appdata_len;
    Ed25519::sign(buf + sig_offset, own_privkey, own_pubkey, msg, mlen);

    return i;
}

// Direct packets are routed without a delay. See getDirectRetransmitDelay(pkt)
static bool no_delay = false;

// ─── Token-bucket duty-cycle helpers ────────────────────────────────────────
// Refill the budget for the elapsed time since the last update.
// Integer fixed-point (no float); 64-bit intermediates for the window math.
static void updateTxBudget(void) {
    uint32_t now = millis();
    uint32_t elapsed = now - last_budget_update;
    if (elapsed == 0) return;
    if (cfg.dutycycle > 0 && cfg.dutycycle < 100) {
        tx_budget_ms += (uint32_t)(((uint64_t)elapsed * cfg.dutycycle) / 100);
        uint32_t max_budget = (uint32_t)(((uint64_t)DUTY_WINDOW_MS * cfg.dutycycle) / 100);
        if (tx_budget_ms > max_budget) tx_budget_ms = max_budget;
    }
    last_budget_update = now;
}

// Block (keeping the watchdog + CLI alive) until the budget covers `airtime` ms,
// capped at DUTY_SILENCE_MAX_MS so a pathological setting can't hang the node.
static void duty_wait(uint32_t airtime) {
    if (cfg.dutycycle <= 0 || cfg.dutycycle >= 100) return;   // unlimited
    uint32_t waited = 0;
    while (tx_budget_ms < airtime && waited < DUTY_SILENCE_MAX_MS) {
        uint32_t needed = airtime - tx_budget_ms;
        uint32_t step   = (uint32_t)(((uint64_t)needed * 100) / cfg.dutycycle); // wall-ms to refill
        if (step < 20)  step = 20;
        if (step > 500) step = 500;      // poll in ≤500 ms slices
        uint32_t t0 = millis();
        while (millis() - t0 < step) { IWatchdog.reload(); cli_poll(); delay(10); }
        waited += step;
        updateTxBudget();
    }
}

// Public accessor for the CLI — refreshes the budget, then reports it
uint32_t get_tx_budget_ms(void) {
    updateTxBudget();
    return tx_budget_ms;
}

// ─── Send a packet (with RF switch + token-bucket duty-cycle gating) ─────────
static bool send_pkt(const uint8_t* buf, int len) {
    rf_idle();

    if (no_delay) {
        delay(10);
        no_delay = false;
    }
    else {
        // Random CSMA backoff 300–800 ms to respect Ch utilization limits
        delay(300 + (millis() % 500));
    }

    // Token bucket: ensure the budget covers this TX before transmitting
    updateTxBudget();
    duty_wait(radio_airtime_ms);

    rf_tx();
    uint32_t tx0   = millis();
    int      state = radio.transmit(const_cast<uint8_t*>(buf), len);
    uint32_t airtime = millis() - tx0;   // actual on-air time
    rf_idle();

    // Consume the measured airtime from the budget
    if (state == RADIOLIB_ERR_NONE && cfg.dutycycle > 0 && cfg.dutycycle < 100) {
        updateTxBudget();
        if (tx_budget_ms > airtime) tx_budget_ms -= airtime;
        else                        tx_budget_ms = 0;
    }
    return (state == RADIOLIB_ERR_NONE);
}

// ─── Process received flood packet ───────────────────────────────────────────
// Returns true if a relay packet should be sent (writes it to relay_buf/relay_len)
#include <stdarg.h>
static void slog(const char* fmt, ...) {
    char buf[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_log(buf);
}

static void slog(const char* fmt, ...);   // fmt-logger → console ring

static uint8_t relay_buf[MAX_PKT];
static int     relay_len = 0;

static bool process_pkt(const uint8_t* data, int len) {
    if (len < 3) { slog("Drop: malformed"); return false; }

    uint8_t header = data[0];
    uint8_t route  = header & 0x03;

    int idx = 1;

    // Path header byte: [hop_count(6) | hash_size_code(2)]
    uint8_t path_hdr  = data[idx++];
    uint8_t hop_count = path_hdr & 0x3F;
    uint8_t hs_code   = (path_hdr >> 6) & 0x03;
    uint8_t hash_size = hs_code + 1;   // 1–4 bytes per hop
    uint8_t ptype     = (header >> 2) & 0x0F;

    int path_bytes = hop_count * hash_size;
    if (idx + path_bytes > len) { slog("Drop: malformed"); return false; }

    const uint8_t* path_ptr    = data + idx;
    const uint8_t* payload_ptr = data + idx + path_bytes;
    int            payload_len = len - idx - path_bytes;

    // ── Neighbor cache: record every ADVERT we hear ──────────────────────────
    if (ptype == PAYLOAD_ADVERT && payload_len >= 40) {
        uint32_t adv_ts;
        memcpy(&adv_ts, payload_ptr + 32, 4);
        int8_t snr_x4 = (int8_t)(radio.getSNR() * 4.0f);
        neighbor_add(payload_ptr, adv_ts, snr_x4);
    }

    // ── DIRECT routing ────────────────────────────────────────────────────────
    // Re-transmit packet UNCHANGED if our hash appears anywhere in the path.
    // (MeshCore reference: repeater.py _handle_direct)
    if (route == ROUTE_DIRECT || route == ROUTE_TRANSPORT_DIRECT) {
        for (int j = 0; j < hop_count; j++) {
            if (path_ptr[j * hash_size] == own_hash) {
                if (len > MAX_PKT) return false;
                relay_len = len;
                memcpy(relay_buf, data, len);

                if (dedup_seen(relay_buf, ptype, (uint8_t)relay_len)) {
                    slog("Drop: dedup direct");
                    return false;
                }

                slog("Relay direct");
                no_delay = true;
                return true;
            }
        }
        if ((ptype == PAYLOAD_TYPE_TRACE) && (own_hash == data[len-1])) {  // PING packet
            if ((payload_len != 10)) return false; // Is it fixed length?
            uint32_t ping_id;
            memcpy(&ping_id, payload_ptr, 4);
            relay_len = 0;
            relay_buf[relay_len++] = ROUTE_DIRECT | (PAYLOAD_TYPE_TRACE<<2);
            relay_buf[relay_len++] = 0x01;
            relay_buf[relay_len++] = (int8_t)(radio.getSNR() * 4.0f);
            memcpy(relay_buf + relay_len, payload_ptr, payload_len);
            relay_len += payload_len;
            slog("Ping: %08lX", (unsigned long)ping_id);
            no_delay = true;
            return true;
        }
        return false;   // Our hash not in path — not our route
    }

    // ── FLOOD routing (ROUTE_FLOOD 0x01 + ROUTE_TRANSPORT_FLOOD 0x00) ────────

    // Guard: relay disabled via CLI (`set repeat off`)
    if (!cfg.repeat_en) { slog("Drop: repeat off"); return false; }

    // Guard: max hops
    if (hop_count >= (cfg.flood_max ? cfg.flood_max : MAX_HOPS)) {
        slog("Drop: max hops"); return false;
    }

    // Guard: payload sanity
    if (payload_len < 1) { slog("Drop: malformed"); return false; }

    // Dedup check
    if (dedup_seen(payload_ptr, ptype, (uint8_t)payload_len)) {
        slog("Drop: dedup flood");
        return false;
    }

    // Loop detection (off = skip; minimal/moderate/strict all drop when our
    // hash appears in the path — 1-byte hashes make finer levels equivalent)
    if (cfg.loop_detect != 0) {
        for (int j = 0; j < hop_count; j++) {
            if (path_ptr[j * hash_size] == own_hash) {
                slog("Drop: our hash in path");
                return false;
            }
        }
    }

    // Build relay packet
    relay_len = 0;
    relay_buf[relay_len++] = header;

    // New path header: hop_count+1 with same hash_size_code
    uint8_t new_hop = hop_count + 1;
    relay_buf[relay_len++] = (uint8_t)((new_hop & 0x3F) | (hs_code << 6));

    // Old path hashes
    if (relay_len + path_bytes + hash_size + payload_len >= MAX_PKT) {
        slog("Drop: relay too large");
        return false;
    }
    memcpy(relay_buf + relay_len, path_ptr, path_bytes);
    relay_len += path_bytes;

    // Append own hash (1..4 bytes depending on hash_size_code)
    relay_buf[relay_len++] = own_hash;
    if (hash_size >= 2) relay_buf[relay_len++] = own_pubkey[1];
    if (hash_size >= 3) relay_buf[relay_len++] = own_pubkey[2];
    if (hash_size >= 4) relay_buf[relay_len++] = own_pubkey[3];

    // Payload
    memcpy(relay_buf + relay_len, payload_ptr, payload_len);
    relay_len += payload_len;

    slog("Relay hop=%u", (unsigned)new_hop);
    return true;
}

// ─── ARDUINO ENTRY POINTS ───────────────────────────────────────────────────

void setup(void) {
    // LED
    pinMode(PIN_LED,  OUTPUT);
    digitalWrite(PIN_LED, LED_ON);   // ON during init

    // RF switch pins
    pinMode(PIN_TXEN, OUTPUT); digitalWrite(PIN_TXEN, LOW);
    pinMode(PIN_RXEN, OUTPUT); digitalWrite(PIN_RXEN, LOW);

    // Serial1 = USART1 = PA9(TX)/PA10(RX) = CH340 USB
    Serial1.begin(115200);
    delay(200);
    IWatchdog.begin(8000000);  // 8 s IWDG — auto-resets if radio HALT or loop freeze

    // ── Configuration: build-flag defaults ← flash prefs overlay ──────────────
    prefs_load();
    cli_begin();

    ts_base = build_unix_ts();
    identity_init();
    Serial1.println();
    Serial1.print("=== SPECTER MeshCore Repeater v");
    Serial1.print(FW_VERSION);
    Serial1.print(" [");
    Serial1.print(cfg.name);
    Serial1.println("] ===");
    Serial1.print("Hash: 0x"); Serial1.println(own_hash, HEX);

    Serial1.print("Radio init...");
    if (radio_init()) {
        radio_ok = true;
        // Start the duty-cycle budget full (as the official Dispatcher does)
        last_budget_update = millis();
        tx_budget_ms = (cfg.dutycycle > 0 && cfg.dutycycle < 100) ?
            (uint32_t)(((uint64_t)DUTY_WINDOW_MS * cfg.dutycycle) / 100) : DUTY_WINDOW_MS;
        Serial1.println(" OK");
        digitalWrite(PIN_LED, LED_OFF);   // OFF, ready
    } else {
        radio_ok = false;
        // Do NOT halt: a bad radio setting (or a transient RF fault) must not
        // brick the serial CLI. Stay in degraded mode — loop() keeps polling
        // the CLI and retries radio_init() until the settings become valid.
        Serial1.println(" FAILED - degraded mode, CLI alive, retrying");
        slog("RADIO init FAILED: degraded mode (fix via CLI, e.g. 'set radio')");
        digitalWrite(PIN_LED, LED_ON);    // solid on = radio not ready
    }
}

static uint32_t last_local_advert = 0;
static uint32_t last_flood_advert = 0;
static bool     first_advert      = false;
static bool     rx_started        = false;
static uint32_t last_beat_ms      = 0;
static bool     advert_pending    = false;      // CLI-triggered advert
static bool     advert_zerohop    = false;
static uint8_t  discovery_burst   = 0;          // zero-hop burst remaining
static uint32_t last_burst_ms     = 0;

// ─── Packet statistics ───────────────────────────────────────────────────────
uint32_t stat_rx    = 0;   // valid packets received from radio
uint32_t stat_relay = 0;   // packets successfully relayed
uint32_t stat_drop  = 0;   // packets rejected (dedup/hops/malformed)
uint32_t stat_advert = 0;  // adverts transmitted

// ─── Console log ring (declared in config.h, implemented in cli.cpp) ─────────
// All runtime diagnostics go here instead of straight to serial; the user
// reads them on demand with the `log` command.
// ─── CLI hooks (declared in config.h, used by cli.cpp) ───────────────────────
void cli_reboot(void) { NVIC_SystemReset(); }

// ─── Programmatic bootloader entry (serial "DFU") ────────────────────────────
// The STM32F103 has no USB DFU. If the board's BOOT0 and RESET lines are
// accessible as GPIOs, define them in platformio.ini:
//   -D DFU_BOOT0_PIN=PB12
//   -D DFU_RST_PIN=PB13
// then `dfu` over the CLI pulls BOOT0 high, pulses RESET low for 100 ms and
// lets go → the MCU reboots into the UART bootloader (57600 8E1 on the same
// USART1/CH340), ready for `stm32flash -w firmware.bin -v -R -b 57600 <port>`.
#ifdef DFU_BOOT0_PIN
void cli_dfu(void) {
    pinMode(DFU_BOOT0_PIN, OUTPUT);
    pinMode(DFU_RST_PIN,   OUTPUT);
    digitalWrite(DFU_RST_PIN, HIGH);    // ensure not held in reset
    delay(20);
    Serial1.println("OK - entering bootloader in 1s");
    delay(1000);                        // let the reply drain through CH340
    digitalWrite(DFU_BOOT0_PIN, HIGH);  // BOOT0=1 on the reset edge
    delay(20);
    digitalWrite(DFU_RST_PIN, LOW);     // pulse NRST
    delay(100);
    digitalWrite(DFU_RST_PIN, HIGH);    // release → reset with BOOT0 high
    while (true) { delay(10); }         // (unreachable; bootloader takes over)
}
#endif

uint32_t cli_now_epoch(void) { return ts_base + millis() / 1000UL; }

void cli_set_time(uint32_t epoch) { ts_base = epoch - millis() / 1000UL; }

const char* cli_version_str(void) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "%s (%s)", FW_VERSION, __DATE__);
    return buf;
}

void cli_send_advert_now(bool zero_hop) {
    if (zero_hop) {
        // Immediate zero-hop advert + short discovery burst
        advert_pending = true;
        advert_zerohop = true;
        discovery_burst = DISCOVERY_BURST;
    } else {
        advert_pending = true;
        advert_zerohop = false;
    }
}

void loop(void) {
    IWatchdog.reload();
    uint32_t now = millis();

    // ── CLI (USB serial) ─────────────────────────────────────────────────────
    cli_poll();   // ALWAYS polled, even in degraded mode, so serial is never bricked

    // ── Radio not ready: retry init periodically, do no radio work ──────────────
    if (!radio_ok) {
        if (now - last_radio_retry >= 3000UL) {
            last_radio_retry = now;
            if (radio_init()) {
                radio_ok     = true;
                rx_started   = false;
                first_advert = false;   // re-send the initial advert once up
                slog("RADIO up after retry");
                digitalWrite(PIN_LED, LED_OFF);
            } else {
                led_blink(3, 120);      // triple-blink = radio still down
            }
        }
        return;
    }

    // ── Heartbeat every 10 s ─────────────────────────────────────────────────
    if (now - last_beat_ms >= 10000UL) {
        last_beat_ms = now;
        slog("ALIVE uptime=%lus ts=%lu rx=%lu relay=%lu drop=%lu",
             (unsigned long)(now / 1000), (unsigned long)cli_now_epoch(),
             (unsigned long)stat_rx, (unsigned long)stat_relay, (unsigned long)stat_drop);
        led_blink(1, 80);   // heartbeat blink every 10 s
    }

    // ── CLI-triggered advert ─────────────────────────────────────────────────
    if (advert_pending) {
        advert_pending = false;
        rx_started = false;
        rf_idle();
        uint8_t abuf[MAX_PKT];
        int     alen = build_advert(abuf);
        stat_advert++;
        slog("ADVERT %s", advert_zerohop ? "zerohop" : "flood");
        digitalWrite(PIN_LED, LED_ON);
        send_pkt(abuf, alen);
        digitalWrite(PIN_LED, LED_OFF);
        last_local_advert = now;   // don't double-send right after
    }

    // ── Discovery burst (discover.neighbors / advert.zerohop) ────────────────
    if (discovery_burst && (now - last_burst_ms) >= DISCOVERY_INTERVAL_MS) {
        last_burst_ms = now;
        discovery_burst--;
        rx_started = false;
        rf_idle();
        uint8_t abuf[MAX_PKT];
        int     alen = build_advert(abuf);
        stat_advert++;
        slog("ADVERT discover %u", (unsigned)discovery_burst);
        digitalWrite(PIN_LED, LED_ON);
        send_pkt(abuf, alen);
        digitalWrite(PIN_LED, LED_OFF);
    }

    // ── ADVERT timers (intervals from cfg, MeshCore standard) ────────────────
    uint32_t local_iv  = (uint32_t)cfg.zerohop_adv_min * 60UL * 1000UL;  // 0 = off
    uint32_t flood_iv  = (uint32_t)cfg.flood_advert_h * 3600UL * 1000UL;

    bool do_advert = false;
    const char* advert_type = "";

    // First ADVERT at boot (flood)
    if (!first_advert && now >= FIRST_ADVERT_MS) {
        do_advert    = true;
        first_advert = true;
        last_flood_advert = now;
        last_local_advert = now;
        advert_type = "initial";
    }
    // Flood ADVERT every N hours (global presence)
    else if (first_advert && (now - last_flood_advert) >= flood_iv) {
        do_advert = true;
        last_flood_advert = now;
        last_local_advert = now;  // sync both timers
        advert_type = "flood";
    }
    // Local (zero-hop) ADVERT every N minutes (neighbor discovery)
    else if (first_advert && local_iv > 0 &&
             (now - last_local_advert) >= local_iv) {
        do_advert = true;
        last_local_advert = now;
        advert_type = "local";
    }

    if (do_advert) {
        rx_started = false;  // restart RX after TX
        rf_idle();
        uint8_t abuf[MAX_PKT];
        int     alen = build_advert(abuf);
        stat_advert++;
        slog("ADVERT %s", advert_type);
        digitalWrite(PIN_LED, LED_ON);
        send_pkt(abuf, alen);
        digitalWrite(PIN_LED, LED_OFF);
    }

    // ── Interrupt-driven receive (DIO1/PC15 fires on RxDone) ─────────────────
    // DIO1 is connected to PC15 on this HW revision, so we use setDio1Action()
    // (registered in radio_init) instead of polling getIrqFlags(). The ISR sets
    // rx_flag; here we react to it. startReceive() arms the radio; on RxDone the
    // ISR fires, we read the packet, and re-arm.
    if (!rx_started) {
        rf_rx();
        int err = radio.startReceive();
        if (err != RADIOLIB_ERR_NONE) {
            slog("startReceive err: %d", (int)err);
        }
        rx_started = true;
    }

    if (rx_flag) {
        rx_flag = false;                      // RxDone
        uint8_t rxbuf[MAX_PKT];
        int state = radio.readData(rxbuf, 0);
        rx_started = false;

        if (state == RADIOLIB_ERR_NONE) {
            int   rxlen = (int)radio.getPacketLength();
            float rssi  = radio.getRSSI();
            int   snr   = (int)radio.getSNR();
            slog("RX RSSI=%d SNR=%d len=%d", (int)rssi, (int)snr, (int)rxlen);
            if (rxlen > 0 && rxlen < MAX_PKT) {
                stat_rx++;
                led_blink(1, 40);         // 1 blink = pkt received
                rf_idle();
                if (process_pkt(rxbuf, rxlen)) {
                    stat_relay++;
                    led_blink(2, 30);     // 2 fast blinks = relay OK
                    send_pkt(relay_buf, relay_len);
                } else {
                    stat_drop++;
                }
            }
        } else {
            // CRC error or other read failure — re-arm and continue
            slog("readData err: %d", (int)state);
        }
    }

    // ── Enter SLEEP until next interrupt ─────────────────────────────────────
    // __WFI() halts the CPU in SLEEP mode until any interrupt fires:
    //   - SysTick (every 1ms) keeps millis() running → ADVERT/heartbeat timing
    //     and IWatchdog.reload() still work (loop re-runs each tick)
    //   - DIO1 (PC15) interrupt wakes on incoming packet → sets rx_flag
    // This reduces CPU power vs. spinning the loop continuously.
    __WFI();
}
