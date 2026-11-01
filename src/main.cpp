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
#include <SHA512.h>
#include <Ed25519.h>
#include <IWatchdog.h>
#include "config.h"
#include "meshcrypto/meshcrypto.h"
#include "meshcrypto/ed_25519.h"
#include "acl.h"

// ─── Version ─────────────────────────────────────────────────────────────────
#define FW_VERSION "1.15.0"

// ─── Mesh-admin payload types (matching MeshCore) ────────────────────────────
#define PAYLOAD_TYPE_REQ        0x00   // Request (dest/src hashes + MAC)
#define PAYLOAD_TYPE_RESPONSE   0x01
#define PAYLOAD_TYPE_TXT_MSG    0x02
#define PAYLOAD_TYPE_ANON_REQ   0x07
// REQ request-type bytes (BaseChatMesh):
#define  REQ_GET_STATS          0x01   // repeater/server stats (get stats)
#define  REQ_GET_TELEMETRY      0x03
#define  REQ_GET_ACL            0x05
#define  REQ_GET_NEIGHBORS      0x06
#define TXT_TYPE_CLI_DATA       1
#define TXT_TYPE_CLI_COMMAND    3
#define RESP_SERVER_LOGIN_OK    0

// ─── Repeater stats (REQ_TYPE_GET_STATUS response) ───────────────────────────
// Byte-exact port of the official simple_repeater RepeaterStats: 56 bytes,
// little-endian, no padding (every field is 2- or 4-byte aligned). The response
// plaintext is [sender_timestamp(4)][RepeaterStats] — the sender's ts is
// reflected back as a correlation 'tag'.
struct RepeaterStats {
    uint16_t batt_milli_volts;
    uint16_t curr_tx_queue_len;
    int16_t  noise_floor;
    int16_t  last_rssi;
    uint32_t n_packets_recv;
    uint32_t n_packets_sent;
    uint32_t total_air_time_secs;
    uint32_t total_up_time_secs;
    uint32_t n_sent_flood, n_sent_direct;
    uint32_t n_recv_flood, n_recv_direct;
    uint16_t err_events;
    int16_t  last_snr;                    // x 4
    uint16_t n_direct_dups, n_flood_dups;
    uint32_t total_rx_air_time_secs;
    uint32_t n_recv_errors;
};
static_assert(sizeof(RepeaterStats) == 56,
              "RepeaterStats must be exactly 56 bytes (byte-exact with the official)");

// Live accumulators (populated in the RX/TX paths; read by handle_req)
static int16_t  stat_last_rssi   = 0;   // last received RSSI (dBm)
static int16_t  stat_last_snr_x4 = 0;   // last received SNR (x4)
static uint32_t stat_tx_air_ms   = 0;   // cumulative TX air time (ms)
static uint32_t stat_rx_air_ms   = 0;   // cumulative RX air time (ms)
static uint32_t stat_sent_flood  = 0;
static uint32_t stat_sent_direct = 0;
static uint32_t stat_recv_flood  = 0;
static uint32_t stat_recv_direct = 0;
static uint32_t stat_direct_dups = 0;
static uint32_t stat_flood_dups  = 0;
static uint32_t stat_recv_errors = 0;   // CRC / read errors

// Basic packet statistics (declared here so handle_req / the stats response can
// use them; the RX/TX paths below increment them)
uint32_t stat_rx     = 0;   // valid packets received from radio
uint32_t stat_relay  = 0;   // packets successfully relayed
uint32_t stat_drop   = 0;   // packets rejected (dedup/hops/malformed)
uint32_t stat_advert = 0;   // adverts transmitted

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
static uint8_t own_x25519_key[32];  // mesh-admin scalar = SHA512(own_privkey)[0:32] clamped
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
    // Derive the X25519 scalar for mesh-admin key exchange. It MUST equal the
    // Ed25519 scalar 'a' used to derive own_pubkey (SHA512(seed)[0:32] clamped),
    // so a companion computing key_exchange(their_a, our_pub) agrees with ours.
    {
        SHA512 sha512;
        sha512.reset();
        sha512.update(own_privkey, 32);
        uint8_t h[64];
        sha512.finalize(h, 64);
        h[0]  &= 0xF8;
        h[31] &= 0x7F;
        h[31] |= 0x40;
        memcpy(own_x25519_key, h, 32);
    }
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

// Short fingerprint (first 4 bytes of SHA256(ptype||payload), 8 hex chars) for
// TX/RX correlation in the log: if an RX fp matches a recent TX fp, that sent
// packet is coming back (self-pickup or a peer echo/ACK).
static void short_fp(const uint8_t* payload, uint8_t ptype, uint8_t plen, char* out) {
    SHA256 sha;
    sha.reset();
    sha.update(&ptype, 1);
    sha.update(payload, plen);
    uint8_t full[32];
    sha.finalize(full, 32);
    snprintf(out, 9, "%02X%02X%02X%02X", full[0], full[1], full[2], full[3]);
}

// Hex dump a buffer (up to `max` bytes) to the log, 16 bytes/line, to inspect
// the content of a dropped / unexpected packet.
static void hex_dump(const uint8_t* buf, int len, int max) {
    if (len > max) len = max;
    for (int off = 0; off < len; off += 16) {
        static char hb[48];
        int n = snprintf(hb, sizeof(hb), "%02d:", off);
        int cnt = (len - off > 16) ? 16 : (len - off);
        for (int i = 0; i < cnt && n < 46; i++)
            n += snprintf(hb + n, sizeof(hb) - n, "%02X", (unsigned)buf[off + i]);
        console_log(hb);
    }
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

static void slog(const char* fmt, ...);   // fwd decl: used by send_pkt's TX log

// ─── Send a packet (with RF switch + token-bucket duty-cycle gating) ─────────
static bool send_pkt(const uint8_t* buf, int len) {
    rf_idle();

    // TX log (RX/TX correlation): ptype, first 2 payload bytes (dest/src for
    // admin, pubkey for advert), hop count, fingerprint. If an RX fp later
    // matches this fp, this packet is coming back to us.
    if (len >= 3) {
        uint8_t ptype    = (buf[0] >> 2) & 0x0F;
        uint8_t path_hdr = buf[1];
        uint8_t hc  = path_hdr & 0x3F;
        uint8_t hs  = ((path_hdr >> 6) & 0x03) + 1;
        int  pb  = hc * hs;
        if (len >= 2 + pb + 2) {
            const uint8_t* pl = buf + 2 + pb;
            char fp[9];
            short_fp(pl, ptype, (uint8_t)(len - 2 - pb), fp);
            slog("TX p=%d d=%02X s=%02X hc=%d fp=%s", ptype, pl[0], pl[1], (int)hc, fp);
        }
    }

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

    // Accumulate TX stats (air time + flood/direct counts)
    stat_tx_air_ms += airtime;
    {
        uint8_t route = buf[0] & 0x03;
        if (route == ROUTE_DIRECT || route == ROUTE_TRANSPORT_DIRECT) stat_sent_direct++;
        else                                                          stat_sent_flood++;
    }

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

// ─── Mesh-admin: build + send a flood reply (encrypted) ──────────────────────
// Packet = [hdr][path_hdr][dest_hash][src_hash][MAC(2)+AES(data)]. Flooding is
// fine for confidentiality: only this node and the client derive `secret`.
static void send_admin_reply(uint8_t ptype, const uint8_t* client_hash, const uint8_t* secret,
                             const uint8_t* data, int data_len) {
    if (!radio_ok) return;
    if (data_len > 150) data_len = 150;
    static uint8_t b[MAX_PKT];   // static: 1 KB stack is too small for this + handlers
    int n = 0;
    b[n++] = (uint8_t)(ROUTE_FLOOD | (ptype << 2));
    b[n++] = 0x00;                  // path_hdr: hop_count=0, hash_size_code=0
    b[n++] = *client_hash;          // dest_hash (the client)
    b[n++] = own_hash;              // src_hash (us)
    mesh_encrypt_then_mac(secret, b + n, data, data_len);   // writes MAC+cipher
    n += 2 + ((data_len + 15) / 16) * 16;
    // Mark our own TX as seen (fingerprint = ptype||payload, matching the
    // official's calculatePacketHash + markSeen). A rebroadcast copy of this
    // reply that loops back through another repeater carries the same payload
    // → same fingerprint, so it's deduped here instead of re-relayed, which is
    // what was producing duplicate replies.
    dedup_seen(b + 2, ptype, (uint8_t)(n - 2));
    no_delay = true;                // small delay, not the 300-800ms relay backoff
    send_pkt(b, n);
}

// Login (ANON_REQ): payload = [dest_hash][sender_pub(32)][MAC+AES(ts(4)+password)]
static bool handle_anon_req(const uint8_t* pl, int plen) {
    if (plen < 1 + 32 + 2 + 1) return false;
    const uint8_t* sender_pub = pl + 1;
    const uint8_t* macData    = pl + 1 + 32;
    int enc_len = plen - 1 - 32;

    uint8_t secret[32];
    ed25519_key_exchange(secret, sender_pub, own_x25519_key);

    static uint8_t data[160];   // static: keep the 1 KB stack small
    int len = mesh_mac_then_decrypt(secret, data, macData, enc_len);
    if (len < 5) return false;
    uint32_t their_ts; memcpy(&their_ts, data, 4);
    const char* password = (const char*)(data + 4);

    uint8_t perms = 0xFF;
    if (password[0] == 0) {
        AclEntry* e = acl_by_pub(sender_pub);
        if (!e) return false;              // blank pw only for an existing session
        perms = e->perms;
    } else if (adm.admin_password[0] && strcmp(password, (const char*)adm.admin_password) == 0) {
        perms = PERM_ADMIN;
    } else if (adm.guest_password[0] && strcmp(password, (const char*)adm.guest_password) == 0) {
        perms = PERM_READ;                  // guest = read-only (no set, no reboot/erase)
    } else {
        return false;                      // bad password
    }

    AclEntry* e = acl_add(sender_pub, perms, secret);
    if (!e) return false;                  // ACL full
    if (their_ts <= e->last_ts) return false;   // login replay guard
    e->perms = perms;
    e->last_ts = their_ts;
    e->last_activity_ms = millis();

    // login RESPONSE: [ts(4)][RESP_SERVER_LOGIN_OK=0][0][is_admin][perms][rand(4)][ver_level]
    static uint8_t resp[13];
    uint32_t now = ts_base + millis() / 1000;
    memcpy(resp, &now, 4);
    resp[4]  = RESP_SERVER_LOGIN_OK;
    resp[5]  = 0;
    resp[6]  = (perms == PERM_ADMIN) ? 1 : 0;
    resp[7]  = perms;
    resp[8]  = (uint8_t)(millis() & 0xFF);
    resp[9]  = (uint8_t)((millis() >> 8) & 0xFF);
    resp[10] = e->pub[0];
    resp[11] = e->pub[1];
    resp[12] = 0;                          // FIRMWARE_VER_LEVEL (informational)
    send_admin_reply(PAYLOAD_TYPE_RESPONSE, sender_pub, secret, resp, sizeof(resp));

    slog("login %02X %s", e->pub[0], perms == PERM_ADMIN ? "admin" : "guest");
    return true;
}

// Minimum permission a CLI command requires (ACL gate). Fail-safe: unknown → ADMIN.
static uint8_t cmd_perm_needed(const char* text) {
    while (*text == ' ' || *text == '\t') text++;
    if (!strncmp(text, "get", 3))       return PERM_READ;
    if (!strncmp(text, "ver", 3))       return PERM_READ;
    if (!strncmp(text, "board", 5))     return PERM_READ;
    if (!strncmp(text, "role", 4))      return PERM_READ;
    if (!strncmp(text, "clock", 5))     return PERM_READ;
    if (!strncmp(text, "time", 4))      return PERM_READ;
    if (!strncmp(text, "stats", 5))     return PERM_READ;
    if (!strncmp(text, "neighbors", 9)) return PERM_READ;
    if (!strncmp(text, "log", 3))       return PERM_READ;
    if (!strncmp(text, "acl", 3))       return PERM_READ;
    if (!strncmp(text, "set", 3))       return PERM_READWRITE;
    // reboot / erase / dfu and anything unrecognized require full admin
    return PERM_ADMIN;
}

// REQ (p=0): payload = [dest_hash][src_hash][MAC+AES(ts(4)+req_type(1)+fields)].
// Decrypt with the session secret, dispatch by request type, and reply with a
// RESPONSE (p=1) = [their_ts(4)][response_data]. Implements REQ_TYPE_GET_STATUS
// (0x01, guest-accessible) → RepeaterStats; other types are logged + dumped.
static bool handle_req(const uint8_t* pl, int plen) {
    if (plen < 2 + 2 + 5) { slog("REQ: short plen=%d", plen); return false; }
    uint8_t src_hash = pl[1];
    AclEntry* e = acl_by_hash(src_hash);
    if (!e) { slog("REQ: no session src=%02X", src_hash); return false; }

    static uint8_t data[160];   // static: keep the 1 KB stack small
    memset(data, 0, sizeof(data));
    int len = mesh_mac_then_decrypt(e->secret, data, pl + 2, plen - 2);
    if (len < 5) { slog("REQ: MAC fail/short src=%02X len=%d", src_hash, len); return false; }

    uint32_t their_ts; memcpy(&their_ts, data, 4);   // request timestamp
    uint8_t req_type = data[4];
    slog("REQ src=%02X type=0x%02X", src_hash, req_type);

    static uint8_t resp[64];
    if (req_type == REQ_GET_STATS) {
        // Reply = [their_ts(4)][RepeaterStats(56)] — reflect the request ts as
        // the correlation tag, then the stats (guest-accessible).
        memcpy(resp, data, 4);
        RepeaterStats s;
        s.batt_milli_volts       = 0;                 // no battery
        s.curr_tx_queue_len      = 0;                 // no TX queue
        s.noise_floor            = 0;                 // not tracked
        s.last_rssi              = stat_last_rssi;
        s.n_packets_recv         = stat_rx;
        s.n_packets_sent         = stat_sent_flood + stat_sent_direct;
        s.total_air_time_secs    = stat_tx_air_ms / 1000;
        s.total_up_time_secs     = millis() / 1000;
        s.n_sent_flood           = stat_sent_flood;
        s.n_sent_direct          = stat_sent_direct;
        s.n_recv_flood           = stat_recv_flood;
        s.n_recv_direct          = stat_recv_direct;
        s.err_events             = 0;                 // no error flags
        s.last_snr               = stat_last_snr_x4;
        s.n_direct_dups          = (uint16_t)stat_direct_dups;
        s.n_flood_dups           = (uint16_t)stat_flood_dups;
        s.total_rx_air_time_secs = stat_rx_air_ms / 1000;
        s.n_recv_errors          = stat_recv_errors;
        memcpy(resp + 4, &s, sizeof(s));
        send_admin_reply(PAYLOAD_TYPE_RESPONSE, &e->pub[0], e->secret, resp, 4 + sizeof(s));
        e->last_activity_ms = millis();
        if (their_ts > e->last_ts) e->last_ts = their_ts;
        return true;
    }
    // Unknown request type: log + dump the request fields, no reply.
    slog("REQ: unhandled type=0x%02X", req_type);
    hex_dump(data + 4, len - 4, 24);
    return true;
}

// TXT_MSG: payload = [dest_hash][src_hash][MAC+AES(ts(4)+flags(1)+text)]
static bool handle_txt_msg(const uint8_t* pl, int plen) {
    if (plen < 2 + 2 + 5) { slog("TXM: short plen=%d", plen); return false; }
    uint8_t src_hash = pl[1];
    AclEntry* e = acl_by_hash(src_hash);
    if (!e) { slog("TXM: no session src=%02X (acl=%d)", src_hash, acl_count()); return false; }   // unknown session

    static uint8_t data[160];   // static: keep the 1 KB stack small
    memset(data, 0, sizeof(data));
    // The sender sizes the datagram to exactly 5+text_len and omits the null
    // terminator; when 5+text_len is a 16-multiple there is no zero padding, so
    // the plaintext is NOT null-terminated. Pre-clearing the buffer guarantees
    // the byte right after the text is a real 0, so strnlen/strchr stop there
    // instead of reading stale bytes from a previous message.
    int len = mesh_mac_then_decrypt(e->secret, data, pl + 2, plen - 2);
    if (len < 5) { slog("TXM: MAC fail src=%02X", src_hash); return false; }
    uint32_t their_ts; memcpy(&their_ts, data, 4);
    uint8_t flags = data[4] >> 2;
    const char* text = (const char*)(data + 5);

    // Diagnostic: hex dump of the FULL decrypted command text. Exposes
    // multi-var / multi-get batches and any non-printable byte separators
    // (20 bytes/line, offset-prefixed; capped at 64 bytes = plenty).
    {
        int tl = (int)strnlen(text, 150);
        if (tl > 64) tl = 64;
        slog("TXM dec=%d txtlen=%d hex:", len, tl);
        for (int off = 0; off < tl; off += 20) {
            static char hb[48];
            int n = snprintf(hb, sizeof(hb), "%02d:", off);
            int cnt = (tl - off > 20) ? 20 : (tl - off);
            for (int i = 0; i < cnt && n < 46; i++)
                n += snprintf(hb + n, sizeof(hb) - n, "%02X", (unsigned)data[5 + off + i]);
            console_log(hb);
        }
    }

    // The app prefixes commands with a short transaction ID ("<sess>|") so it
    // can match each reply to the request that produced it. Strip it, run the
    // command, and echo it back in the reply ("<sess>|<reply>"). No '|', or a
    // run of >8 chars before it, is treated as a plain (unprefixed) command.
    static char sess[16];
    int sess_len = 0;
    const char* cmd = text;
    {
        const char* p = strchr(text, '|');
        if (p && (int)(p - text) >= 1 && (int)(p - text) <= 8) {
            sess_len = (int)(p - text);
            memcpy(sess, text, sess_len);
            sess[sess_len] = '\0';
            cmd = p + 1;
        }
    }
    slog("TXM: ok src=%02X fl=%d sess='%s' cmd='%s'", src_hash, flags, sess_len ? sess : "-", cmd);

    // The app sends commands as TXT_TYPE_CLI_DATA (1) or TXT_TYPE_CLI_COMMAND (3);
    // both mean "run this". Genuine command *replies* are addressed to the client,
    // never to us, so accepting both can't mis-fire here.
    if (flags != TXT_TYPE_CLI_COMMAND && flags != TXT_TYPE_CLI_DATA) {
        slog("TXM: not cmd fl=%d", flags); return false;
    }
    if (e->perms < cmd_perm_needed(cmd)) { slog("TXM: perms deny (p=%d)", e->perms); return false; }

    static char reply[150];
    int rlen = cli_execute(cmd, reply, sizeof(reply));
    int prefix_len = sess_len ? (sess_len + 1) : 0;   // "<sess>|"
    if (rlen > 148 - prefix_len) rlen = 148 - prefix_len;   // keep under the 150-byte cap

    // reply as TXT_MSG CLI_DATA: [ts(4)][flags=CLI_DATA<<2][<sess>|]<reply>
    static uint8_t out[165];
    uint32_t now = ts_base + millis() / 1000;
    memcpy(out, &now, 4);
    out[4] = (uint8_t)(TXT_TYPE_CLI_DATA << 2);
    int o = 5;
    if (prefix_len) { memcpy(out + o, sess, sess_len); out[o + sess_len] = '|'; o += prefix_len; }
    memcpy(out + o, reply, rlen);
    slog("TXM: reply %d bytes (prefix=%d)", rlen, prefix_len);
    send_admin_reply(PAYLOAD_TYPE_TXT_MSG, &e->pub[0], e->secret, out, o + rlen);

    e->last_activity_ms = millis();
    if (their_ts > e->last_ts) e->last_ts = their_ts;
    return true;
}

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

    // RX log (RX/TX correlation): ptype, first 2 payload bytes (dest/src for
    // admin, pubkey for advert), hop count, fingerprint. If this fp matches a
    // recent TX fp, our own packet is coming back (self-pickup or a peer echo).
    if (payload_len >= 2) {
        char fp[9];
        short_fp(payload_ptr, ptype, (uint8_t)payload_len, fp);
        slog("RX p=%d d=%02X s=%02X hc=%d fp=%s", ptype, payload_ptr[0], payload_ptr[1], (int)hop_count, fp);
    }

    // ── Neighbor cache: record every ADVERT we hear ──────────────────────────
    if (ptype == PAYLOAD_ADVERT && payload_len >= 40) {
        uint32_t adv_ts;
        memcpy(&adv_ts, payload_ptr + 32, 4);
        int8_t snr_x4 = (int8_t)(radio.getSNR() * 4.0f);
        neighbor_add(payload_ptr, adv_ts, snr_x4);
    }

    // ── Mesh-admin packets addressed to us (dest_hash == own_hash) ──────────
    // Handled (or dropped) here; never relayed. Packets not addressed to us fall
    // through and are relayed normally (propagating a login to other repeaters).
    if (ptype == PAYLOAD_TYPE_ANON_REQ || ptype == PAYLOAD_TYPE_TXT_MSG || ptype == PAYLOAD_TYPE_RESPONSE || ptype == PAYLOAD_TYPE_REQ) {
        // DIAGNOSTIC: log admin packets + whether they're addressed to us
        if (ptype == PAYLOAD_TYPE_ANON_REQ)
            slog("ANON dst=%02X src? own=%02X hc=%d %s", payload_ptr[0], own_hash, hop_count, payload_ptr[0]==own_hash?"US":"relay");
        else if (ptype == PAYLOAD_TYPE_TXT_MSG && payload_len >= 2)
            slog("TXM  dst=%02X src=%02X own=%02X hc=%d %s", payload_ptr[0], payload_ptr[1], own_hash, hop_count, payload_ptr[0]==own_hash?"US":"relay");
        else if (ptype == PAYLOAD_TYPE_REQ && payload_len >= 2)
            slog("REQ  dst=%02X src=%02X own=%02X hc=%d %s", payload_ptr[0], payload_ptr[1], own_hash, hop_count, payload_ptr[0]==own_hash?"US":"relay");
        if (payload_len >= 2 && payload_ptr[0] == own_hash) {
            // Record + dedup the addressed-to-us packet. This block returns
            // before the flood-relay dedup below, so without this a re-relayed
            // copy of a command (same payload -> same fingerprint) that loops
            // back through another repeater would be reprocessed, re-sending the
            // reply — the duplicate the companion was seeing.
            if (dedup_seen(payload_ptr, ptype, (uint8_t)payload_len)) {
                slog("Drop: dedup admin");
                return false;
            }
            if (ptype == PAYLOAD_TYPE_ANON_REQ) handle_anon_req(payload_ptr, payload_len);
            else if (ptype == PAYLOAD_TYPE_TXT_MSG) handle_txt_msg(payload_ptr, payload_len);
            else if (ptype == PAYLOAD_TYPE_REQ) handle_req(payload_ptr, payload_len);
            return false;
        }
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
                    stat_direct_dups++;
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

    // Self-pickup guard: with a detached / weak antenna we can RX our own TX.
    // A few bits flip in transit, so the payload (and fingerprint) differ from
    // the markSeen'd original — the dedup misses it and we'd relay our own
    // reply. If an admin packet's src is us, it's our own transmission: drop
    // it (and hex-dump it so we can confirm it's a corrupted self-pickup).
    if ((ptype == PAYLOAD_TYPE_TXT_MSG || ptype == PAYLOAD_TYPE_ANON_REQ || ptype == PAYLOAD_TYPE_RESPONSE)
        && payload_len >= 2 && payload_ptr[1] == own_hash) {
        slog("Drop: self-pickup src=us fp");
        hex_dump(payload_ptr, payload_len, 24);
        return false;
    }

    // Dedup check
    if (dedup_seen(payload_ptr, ptype, (uint8_t)payload_len)) {
        stat_flood_dups++;
        slog("Drop: dedup flood");
        hex_dump(payload_ptr, payload_len, 24);
        return false;
    }

    // Loop detection (off = skip; minimal/moderate/strict all drop when our
    // hash appears in the path — 1-byte hashes make finer levels equivalent)
    if (cfg.loop_detect != 0) {
        for (int j = 0; j < hop_count; j++) {
            if (path_ptr[j * hash_size] == own_hash) {
                slog("Drop: our hash in path");
                hex_dump(payload_ptr, payload_len, 24);
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
    // Boot signature: 5 quick flashes so a successful *app* boot is easy to tell
    // apart from the ROM bootloader (which shows a SOLID LED). Fires before radio
    // init, so you get it even if the radio later comes up in degraded mode.
    led_blink(5, 70);
    digitalWrite(PIN_LED, LED_OFF);

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

    // ── Expire idle mesh-admin sessions (admin.timeout seconds) ──────────────
    {
        static uint32_t last_acl_ms = 0;
        if (adm.admin_timeout > 0 && now - last_acl_ms >= 1000UL) {
            last_acl_ms = now;
            uint32_t lim = (uint32_t)adm.admin_timeout * 1000UL;
            for (int i = 0; i < ACL_MAX; i++) {
                AclEntry* e = (AclEntry*)acl_at(i);
                if (e && e->active && (now - e->last_activity_ms) > lim) {
                    e->active = 0;
                    slog("session %02X expired", e->pub[0]);
                }
            }
        }
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
            stat_last_rssi   = (int16_t)rssi;          // last RX signal level
            stat_last_snr_x4 = (int16_t)(snr * 4);     // last SNR, x4
            stat_rx_air_ms  += radio_airtime_ms;       // rough per-packet RX air time
            if (rxlen > 0 && rxlen < MAX_PKT) {
                stat_rx++;
                uint8_t route = rxbuf[0] & 0x03;
                if (route == ROUTE_DIRECT || route == ROUTE_TRANSPORT_DIRECT) stat_recv_direct++;
                else                                                          stat_recv_flood++;
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
            stat_recv_errors++;   // CRC error or other read failure
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
