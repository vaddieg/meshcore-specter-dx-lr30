/*
 * SPECTER CLI — USB-serial command line (MeshCore subset, see docs/cli.md).
 *
 * Line-based, non-blocking: cli_poll() drains available bytes, dispatches
 * each complete line. Commands are case-insensitive. Replies follow the
 * MeshCore style: bare value / "OK" / "Err: ...".
 */
#include "config.h"

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <string.h>
#include <stdlib.h>

// Radio object lives in main.cpp
extern SX1262 radio;

// ─── Line reader ────────────────────────────────────────────────────────────
// Console log ring: diagnostics captured, printed on demand via `log`
#define LOG_LINES 32
#define LOG_LEN   48
static char    log_buf[LOG_LINES][LOG_LEN];
static uint8_t log_head = 0;    // next slot to write
static uint16_t log_count = 0;  // lines stored (<= LOG_LINES)

void console_log(const char* line) {
    strncpy(log_buf[log_head], line, LOG_LEN - 1);
    log_buf[log_head][LOG_LEN - 1] = '\0';
    log_head = (uint8_t)((log_head + 1) % LOG_LINES);
    if (log_count < LOG_LINES) log_count++;
}

void console_clear(void) {
    memset(log_buf, 0, sizeof(log_buf));
    log_head = 0;
    log_count = 0;
}

// Copy up to max_lines most-recent lines into out_lines (line_len each)
int console_recent(char* out_lines, int max_lines, int line_len) {
    int n = log_count < max_lines ? (int)log_count : max_lines;
    for (int i = 0; i < n; i++) {
        int idx = (log_head - n + i + 2 * LOG_LINES) % LOG_LINES;
        strncpy(out_lines + i * line_len, log_buf[idx], (size_t)line_len - 1);
        out_lines[i * line_len + line_len - 1] = '\0';
    }
    return n;
}

static char  line[96];
static uint8_t line_len = 0;

// Pristine copy of the current line for handlers that need the full line
// (set name / set radio) - the tokenizer null-terminates in place.
static char line_raw[96];

static void handle_line(char* raw);   // fwd

// A line was just dispatched on the previous byte (CR); the LF of a CRLF
// pair must not dispatch again.
static bool line_closed = false;

void cli_begin(void) {
    line_len = 0;
}

// Official repeater CLI wire protocol (examples/simple_repeater/main.cpp and
// config.meshcore.io lib/serial-cli.js):
//   - every received byte is echoed back (local echo for dumb terminals)
//   - CR terminates the command (a following LF is ignored)
//   - replies are prefixed "  -> " and CRLF-terminated
//   - multi-line `log` output ends with "   EOF" on its own line
void cli_poll(void) {
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\r') {
            Serial1.write(c);                       // echo CR
            line[line_len] = '\0';
            handle_line(line);
            line_len = 0;
            line_closed = true;
        } else if (c == '\n') {
            if (line_closed) { line_closed = false; }
            else             { line[line_len] = '\0'; handle_line(line); line_len = 0; }
        } else if (c == 0x7F || c == 0x08) {
            Serial1.write(c);                       // echo backspace
            if (line_len > 0) line_len--;
        } else {
            Serial1.write(c);                       // echo input
            if (line_len < (int)sizeof(line) - 1) line[line_len++] = c;
        }
    }
}

// ─── Helpers ────────────────────────────────────────────────────────────────
// Reply: "  -> <text>\r\n"  (marker + CRLF required by config.meshcore.io)
static void out(const char* s) {
    Serial1.print("  -> ");
    Serial1.print(s);
    Serial1.print("\r\n");
}

// Numeric reply (out() is const char*; this formats an unsigned long)
static void out_num(unsigned long v) {
    Serial1.print("  -> ");
    Serial1.print(v);
    Serial1.print("\r\n");
}

// `get` replies: "  -> > <text>\r\n". The config.meshcore.io parser
// (parseVariableResponse) requires the "  -> " marker AND a "> " prefix on
// the payload (it does response.startsWith("> ") then strips it). The official
// firmware formats every get reply as "> <value>", so the full line is
// "  -> > <value>". Without the "> " the app treats the var as unsupported.
static void getv(const char* s) {
    Serial1.print("  -> > ");
    Serial1.print(s);
    Serial1.print("\r\n");
}
static void getv_num(unsigned long v) {
    Serial1.print("  -> > ");
    Serial1.print(v);
    Serial1.print("\r\n");
}

// One raw line of multi-line output, CRLF-terminated (used by `log`)
static void raw_line(const char* s) {
    Serial1.print(s);
    Serial1.print("\r\n");
}

static int parse_int(const char* s, long* out) {
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}

// Parse "N.NNN" (1-6 decimals) → int32 micro-units (newlib-nano has no
// float printf/scanf, so everything is done in integer fixed point)
static int parse_micro(const char* s, int32_t* out) {
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') s++;
    const char* p = s;
    if (*p < '0' || *p > '9') return -1;
    int32_t ip = 0;
    while (*p >= '0' && *p <= '9') { ip = ip * 10 + (*p - '0'); p++; }
    int32_t frac = 0;
    int nd = 0;
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return -1;
        while (*p >= '0' && *p <= '9' && nd < 6) { frac = frac * 10 + (*p - '0'); p++; nd++; }
        while (*p >= '0' && *p <= '9') p++;   // ignore extras
    }
    while (nd++ < 6) frac *= 10;
    // Tolerate a single trailing ".<digits>" (e.g. the config app sends
    // "62.5.0" for a 62.5 kHz bandwidth, since it appends ".0"). Behave like
    // C strtof, which stops at the second dot and ignores the remainder.
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p != '\0') return -1;
    int32_t v = ip * 1000000 + frac;
    if (neg) v = -v;
    *out = v;
    return 0;
}

// Parse "N.NNN" degrees, 1-6 decimals → int32 micro-degrees
static int parse_degrees(const char* s, int32_t* out) {
    int32_t v;
    if (parse_micro(s, &v)) return -1;
    if (v < -90000000 || v > 90000000) return -1;
    *out = v;
    return 0;
}

// ─── Neighbor cache (8 most recent adverts) ─────────────────────────────────
#define NEIGH_MAX 8
static struct {
    uint8_t  pubkey[32];
    uint32_t ts;
    int8_t   snr_x4;
    bool     used;
} nbrs[NEIGH_MAX];
static uint8_t nbr_idx = 0;

void neighbor_add(const uint8_t* pubkey32, uint32_t ts, int8_t snr_x4) {
    // Update in place if we already track this key
    for (int i = 0; i < NEIGH_MAX; i++) {
        if (nbrs[i].used && memcmp(nbrs[i].pubkey, pubkey32, 32) == 0) {
            nbrs[i].ts = ts;
            nbrs[i].snr_x4 = snr_x4;
            return;
        }
    }
    // Otherwise overwrite the oldest slot (round-robin ring)
    int slot = nbr_idx;
    nbr_idx = (uint8_t)((nbr_idx + 1) % NEIGH_MAX);
    nbrs[slot].used = true;
    memcpy(nbrs[slot].pubkey, pubkey32, 32);
    nbrs[slot].ts = ts;
    nbrs[slot].snr_x4 = snr_x4;
}

// ─── Command dispatch ───────────────────────────────────────────────────────
static void cmd_ver(void)       { out(cli_version_str()); }
static void cmd_board(void)     { out("DX-LR30"); }
static void cmd_role(void)      { getv("repeater"); }
static void cmd_reboot(void)    { cli_reboot(); }
static void cmd_clock(void)     { out_num(cli_now_epoch()); }

static void cmd_erasectl(void)  {
    prefs_erase();
    out("OK");
    delay(100);
    cli_reboot();
}

static void cmd_time(int argc, char** argv) {
    if (argc < 2) { out("Err: usage: time <epoch_seconds>"); return; }
    long v;
    if (parse_int(argv[1], &v) || v < 946684800L /*2000-01-01*/) {
        out("Err: bad epoch");
        return;
    }
    cli_set_time((uint32_t)v);
    out("OK");
}

static void cmd_advert(int argc, char** argv) {
    bool zero_hop = (argc >= 2 && strcmp(argv[1], "zerohop") == 0);
    cli_send_advert_now(zero_hop);
    out("OK");
}

static void cmd_neighbors(void) {
    for (int i = 0; i < NEIGH_MAX; i++) {
        if (!nbrs[i].used) continue;
        char buf[40];
        snprintf(buf, sizeof(buf), "%02X%02X:%u:%d",
                 nbrs[i].pubkey[0], nbrs[i].pubkey[1],
                 (unsigned)nbrs[i].ts, (int)nbrs[i].snr_x4);
        out(buf);
    }
}

extern uint32_t get_tx_budget_ms(void);

static void cmd_stats_core(void) {
    char buf[100];
    snprintf(buf, sizeof(buf), "uptime=%lus name=%s duty=%u%% budget=%lums loop.detect=%s",
             (unsigned long)(millis() / 1000), cfg.name,
             (unsigned)cfg.dutycycle, (unsigned long)get_tx_budget_ms(),
             cfg.loop_detect ? "on" : "off");
    out(buf);
}

// extern counters from main.cpp
extern uint32_t stat_rx, stat_relay, stat_drop, stat_advert;

static void cmd_stats_packets(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "rx=%lu relay=%lu drop=%lu advert=%lu",
             (unsigned long)stat_rx, (unsigned long)stat_relay,
             (unsigned long)stat_drop, (unsigned long)stat_advert);
    out(buf);
}

// ─── get / set ──────────────────────────────────────────────────────────────
static void get_name(void)     { getv(cfg.name); }
static void set_name(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set name <name>"); return; }
    // Everything after "set name " is the name (may contain spaces)
    const char* p = strstr(line_raw, "set name");
    if (!p) p = strstr(line_raw, "SET NAME");
    if (!p) p = strstr(line_raw, "Set name");
    if (!p) { out("Err: parse"); return; }
    p += 8;
    while (*p == ' ') p++;
    int nlen = (int)strlen(p);
    if (nlen < 1) { out("Err: empty name"); return; }
    // Advert limit: 23 B with location, 31 B without
    int max = (cfg.lat_micro || cfg.lon_micro) ? 23 : 31;
    if (nlen > max) {
        out("Err: name too long (max 23 with location, 31 without)");
        return;
    }
    memcpy(cfg.name, p, (size_t)nlen + 1);
    prefs_save();
    out("OK");
}

// Fixed-point formatter (newlib-nano %f is stubbed out on this platform);
// builds into buf, returns pointer (caller prints via out()).
static const char* fmt_fixed(char* buf, size_t n, int32_t x, int decimals) {
    bool neg = x < 0;
    uint32_t v = neg ? (uint32_t)(-x) : (uint32_t)x;
    uint32_t scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    int o = 0;
    if (neg) buf[o++] = '-';
    o += snprintf(buf + o, n - o, "%lu.", (unsigned long)(v / scale));
    uint32_t frac = v % scale;
    for (uint32_t m = scale / 10; m > 0; m /= 10) buf[o++] = '0' + (frac / m) % 10;
    buf[o] = '\0';
    return buf;
}

static void get_lat(void) { char b[24]; getv(fmt_fixed(b, sizeof(b), cfg.lat_micro, 6)); }
static void get_lon(void) { char b[24]; getv(fmt_fixed(b, sizeof(b), cfg.lon_micro, 6)); }
static void set_coord(int argc, char** argv, int is_lat) {
    if (argc < 3) { out("Err: usage: set lat|lon <degrees>"); return; }
    int32_t v;
    if (parse_degrees(argv[2], &v)) { out("Err: bad degrees"); return; }
    if (is_lat) cfg.lat_micro = v; else cfg.lon_micro = v;
    prefs_save();
    out("OK");
}

static void get_radio(void) {
    char buf[48];
    // freq: MHz with 3 decimals (khz/1000 → 1e-3 MHz); bw: kHz with 1 decimal
    int32_t f_frac = (int32_t)cfg.freq_khz % 1000;
    int32_t b_frac = (int32_t)(cfg.bw_tenth % 10);
    snprintf(buf, sizeof(buf), "%lu.%03lu,%lu.%ld,%u,%u",
             (unsigned long)(cfg.freq_khz / 1000), (unsigned long)f_frac,
             (unsigned long)(cfg.bw_tenth / 10), (long)b_frac,
             (unsigned)cfg.sf, (unsigned)cfg.cr);
    getv(buf);
}

// Parse "freq,bw,sf,cr" from raw remainder (integer fixed point)
static bool parse_radio(const char* s, uint32_t* khz, uint16_t* bw,
                        uint8_t* sf, uint8_t* cr) {
    char buf[64];
    int n = (int)strlen(s);
    if (n == 0 || n >= (int)sizeof(buf)) return false;
    memcpy(buf, s, (size_t)n + 1);
    int32_t f_mhz, b_khz_tenth;
    long sfi, cri;
    char* t[4];
    int cnt = 0;
    t[cnt++] = buf;
    for (int i = 0; i < n && cnt < 4; i++) {
        if (buf[i] == ',') { buf[i] = '\0'; t[cnt++] = buf + i + 1; }
    }
    if (cnt != 4) return false;
    if (parse_micro(t[0], &f_mhz)) return false;              // MHz → micro
    if (parse_micro(t[1], &b_khz_tenth)) return false;        // kHz → micro
    if (parse_int(t[2], &sfi)) return false;
    if (parse_int(t[3], &cri)) return false;
    if (f_mhz < 150000000 || f_mhz > 960000000) return false;    // 150–960 MHz (SX1262)
    if (b_khz_tenth < 7800000 || b_khz_tenth > 500000000) return false; // 7.8–500 kHz
    if (sfi < 5 || sfi > 12) return false;                        // SF 5–12
    if (cri < 4 || cri > 8)  return false;                        // CR 4–8 (SX1262)
    *khz = (uint32_t)(f_mhz / 1000);                          // micro-MHz → kHz
    *bw  = (uint16_t)(b_khz_tenth / 100000);                  // micro-kHz → kHz×10
    *sf  = (uint8_t)sfi;
    *cr  = (uint8_t)cri;
    return true;
}

static void set_radio(int argc, char** argv) {
    (void)argc;
    const char* p = strstr(line_raw, "set radio");
    if (!p) p = strstr(line_raw, "SET RADIO");
    if (!p) { out("Err: parse"); return; }
    p += 9;
    while (*p == ' ') p++;
    uint32_t khz; uint16_t bw; uint8_t sf, cr;
    if (!parse_radio(p, &khz, &bw, &sf, &cr)) {
        out("Err: usage: set radio <freq>,<bw>,<sf>,<cr>");
        return;
    }
    cfg.freq_khz = khz; cfg.bw_tenth = bw; cfg.sf = sf; cfg.cr = cr;
    prefs_save();
    out("OK, reboot to apply");
}

static void get_freq(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu.%03lu",
             (unsigned long)(cfg.freq_khz / 1000),
             (unsigned long)((uint32_t)cfg.freq_khz % 1000));
    getv(buf);
}
static void set_freq(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set freq <MHz>"); return; }
    int32_t f_mhz;
    if (parse_micro(argv[2], &f_mhz) ||
        f_mhz < 300000000 || f_mhz > 2500000000) {
        out("Err: bad frequency");
        return;
    }
    cfg.freq_khz = (uint32_t)(f_mhz / 1000);   // micro-MHz → kHz
    prefs_save();
    out("OK, reboot to apply");
}

static void get_tx(void)       { getv_num(cfg.tx_dbm); }
static void set_tx(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set tx <dbm>"); return; }
    long v;
    if (parse_int(argv[2], &v) || v < 1 || v > 22) { out("Err: 1-22"); return; }
    cfg.tx_dbm = (uint8_t)v;
    prefs_save();
    out("OK, reboot to apply");
}

static void get_flood_adv(void)  { getv_num(cfg.flood_advert_h); }
static void set_flood_adv(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set flood.advert.interval <hours>"); return; }
    long v;
    if (parse_int(argv[2], &v) || v < 3 || v > 168) { out("Err: 3-168"); return; }
    cfg.flood_advert_h = (uint8_t)v;
    prefs_save();
    out("OK");
}

static void get_zero_adv(void)   { getv_num(cfg.zerohop_adv_min); }
static void set_zero_adv(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set advert.interval <minutes>"); return; }
    long v;
    if (parse_int(argv[2], &v) || v < 0) { out("Err: bad minutes"); return; }
    if (v > 0 && v < 60) v = 0;                 // below min → disabled
    if (v > 240)         v = 240;
    v &= ~1L;                                    // round down to multiple of 2
    cfg.zerohop_adv_min = (uint16_t)v;
    prefs_save();
    out("OK");
}

static void get_public_key(void) {
    extern uint8_t own_pubkey[32];
    char buf[65];
    for (int i = 0; i < 32; i++)
        sprintf(buf + 2 * i, "%02X", own_pubkey[i]);
    getv(buf);
}

static void get_repeat(void)    { getv(cfg.repeat_en ? "on" : "off"); }
static void set_repeat(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set repeat on|off"); return; }
    if (strcmp(argv[2], "on") == 0)       cfg.repeat_en = 1;
    else if (strcmp(argv[2], "off") == 0) cfg.repeat_en = 0;
    else { out("Err: on|off"); return; }
    prefs_save();
    out("OK");
}

static void get_loop_detect(void) {
    static const char* names[] = {"off", "minimal", "moderate", "strict"};
    getv(names[cfg.loop_detect & 3]);
}
static void set_loop_detect(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set loop.detect off|minimal|moderate|strict"); return; }
    if      (strcmp(argv[2], "off") == 0)      cfg.loop_detect = 0;
    else if (strcmp(argv[2], "minimal") == 0)  cfg.loop_detect = 1;
    else if (strcmp(argv[2], "moderate") == 0) cfg.loop_detect = 2;
    else if (strcmp(argv[2], "strict") == 0)   cfg.loop_detect = 3;
    else { out("Err: off|minimal|moderate|strict"); return; }
    prefs_save();
    out("OK");
}

static void get_txdelay(void) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u",
             (unsigned)(cfg.txdelay_x10 / 10), (unsigned)(cfg.txdelay_x10 % 10));
    getv(buf);
}
static void set_txdelay(int argc, char** argv, bool direct) {
    if (argc < 3) { out("Err: usage: set txdelay|direct.txdelay <0-2>"); return; }
    int32_t v;
    if (parse_micro(argv[2], &v) || v < 0 || v > 2000000) { out("Err: 0-2"); return; }
    uint8_t x10 = (uint8_t)(v / 100000);   // micro → tenths
    if (direct) cfg.direct_txdelay_x10 = x10;
    else        cfg.txdelay_x10        = x10;
    prefs_save();
    out("OK");
}

static void get_dutycycle(void) { getv_num(cfg.dutycycle); }
static void set_dutycycle(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set dutycycle <1-100>"); return; }
    long v;
    if (parse_int(argv[2], &v) || v < 1 || v > 100) { out("Err: 1-100"); return; }
    cfg.dutycycle = (uint8_t)v;
    prefs_save();
    out("OK");
}

// Airtime factor — the app's 'af' variable (duty% = 100/(af+1)). We store
// dutycycle as a whole percent (1-100), so bridge the two. The app only sends
// 'nice' af values derived from its 1-50% slider, so the round-trip is exact.
static void get_af(void) {
    int dc = cfg.dutycycle < 1 ? 1 : (int)cfg.dutycycle;
    long af_x10 = (1000L / dc) - 10L;          // af*10 (1 decimal)
    char b[16];
    getv(fmt_fixed(b, sizeof(b), (int32_t)af_x10, 1));
}
static void set_af(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set af <factor>"); return; }
    int32_t af_micro;
    if (parse_micro(argv[2], &af_micro) || af_micro < 0) { out("Err: bad af"); return; }
    long denom = (long)af_micro + 1000000L;     // (af+1) in micro
    long duty = (100000000L + denom / 2) / denom;   // round(100/(af+1))
    if (duty < 1)   duty = 1;
    if (duty > 100) duty = 100;
    cfg.dutycycle = (uint8_t)duty;
    prefs_save();
    out("OK");
}

static void get_flood_max(void) { getv_num(cfg.flood_max); }
static void set_flood_max(int argc, char** argv) {
    if (argc < 3) { out("Err: usage: set flood.max <0-64>"); return; }
    long v;
    if (parse_int(argv[2], &v) || v < 0 || v > 64) { out("Err: 0-64"); return; }
    cfg.flood_max = (uint8_t)v;
    prefs_save();
    out("OK");
}


static void handle_line(char* raw) {
    // Trim leading/trailing whitespace
    while (*raw == ' ' || *raw == '\t') raw++;
    int rawlen = (int)strlen(raw);
    if (rawlen >= (int)sizeof(line_raw)) rawlen = sizeof(line_raw) - 1;
    memcpy(line_raw, raw, (size_t)rawlen);
    line_raw[rawlen] = '\0';
    char* endp = raw + strlen(raw);
    while (endp > raw && (endp[-1] == ' ' || endp[-1] == '\t')) *--endp = '\0';
    if (*raw == '\0' || *raw == '#') { return; }

    // Tokenize (max 4 tokens); argv[0] = command
    char* argv[5] = {0};
    int argc = 0;
    char* p = raw;
    while (argc < 4) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = nullptr;

    const char* c = argv[0];
    if (argc == 0) { return; }

    // Each matched command re-prints the prompt afterwards (FINISH);
    // reboot/erase/dfu never return, so no prompt needed there.
    bool done = false;
    #define FINISH(cmd) do { cmd; done = true; } while (0)

    // Single-word commands
    if (!strcmp(c, "ver") || !strcmp(c, "ver\n"))          { FINISH(cmd_ver()); }
    else if (!strcmp(c, "board"))                           { FINISH(cmd_board()); }
    else if (!strcmp(c, "role"))                            { FINISH(cmd_role()); }
    else if (!strcmp(c, "reboot"))                          { cmd_reboot(); }
#ifdef DFU_BOOT0_PIN
    else if (!strcmp(c, "dfu"))                             { cli_dfu(); }
#endif
    else if (!strcmp(c, "erase"))                           { cmd_erasectl(); }
    else if (!strcmp(c, "clock"))                           { FINISH(cmd_clock()); }
    else if (!strcmp(c, "neighbors"))                       { FINISH(cmd_neighbors()); }
    else if (!strcmp(c, "log")) {
        if (argc >= 2 && (!strcmp(argv[1], "clear") || !strcmp(argv[1], "erase"))) {
            console_clear();
            out("OK");
            done = true;
        } else {
            char outbuf[20 * 48];
            int n = console_recent(outbuf, 20, 48);
            if (n == 0) raw_line("(log empty)");
            for (int i = 0; i < n; i++) raw_line(outbuf + i * 48);
            raw_line("   EOF");            // multi-line terminator (parser)
            done = true;
        }
    }
    else if (!strcmp(c, "discover")) {
        if (argc >= 2 && !strcmp(argv[1], "neighbors")) { cli_send_advert_now(true); out("OK"); }
        done = true;
    }
    else if (!strcmp(c, "stats-core"))                      { FINISH(cmd_stats_core()); }
    else if (!strcmp(c, "stats-packets"))                   { FINISH(cmd_stats_packets()); }
    else if (!strcmp(c, "time"))                            { cmd_time(argc, argv); done = true; }
    else if (!strcmp(c, "advert"))                          { cmd_advert(argc, argv); done = true; }

    // "get <thing>"
    else if (!strcmp(c, "get")) {
        done = true;   // all get/set paths print the prompt afterwards
        if (argc < 2) { out("Err: usage: get <name|lat|lon|radio|freq|tx|public.key|role|...>"); }
        const char* g = argv[1];
        if      (!strcmp(g, "name"))        get_name();
        else if (!strcmp(g, "role"))        cmd_role();
        else if (!strcmp(g, "lat"))         get_lat();
        else if (!strcmp(g, "lon"))         get_lon();
        else if (!strcmp(g, "radio"))       get_radio();
        else if (!strcmp(g, "freq"))        get_freq();
        else if (!strcmp(g, "tx"))          get_tx();
        else if (!strcmp(g, "flood.advert.interval")) get_flood_adv();
        else if (!strcmp(g, "advert.interval"))       get_zero_adv();
        else if (!strcmp(g, "public.key"))  get_public_key();
        else if (!strcmp(g, "repeat"))      get_repeat();
        else if (!strcmp(g, "loop.detect")) get_loop_detect();
        else if (!strcmp(g, "txdelay"))     get_txdelay();
        else if (!strcmp(g, "direct.txdelay")) { char b[16]; snprintf(b,sizeof(b),"%u.%u",(unsigned)(cfg.direct_txdelay_x10/10),(unsigned)(cfg.direct_txdelay_x10%10)); getv(b); }
        else if (!strcmp(g, "dutycycle"))   get_dutycycle();
        else if (!strcmp(g, "af"))          get_af();
        else if (!strcmp(g, "flood.max"))   get_flood_max();
        else out("Err: unknown (name|lat|lon|radio|freq|tx|public.key|role|repeat|loop.detect|txdelay|direct.txdelay|dutycycle|flood.max|flood.advert.interval|advert.interval)");
        done = true;
    }

    // "set <thing> ..."
    else if (!strcmp(c, "set")) {
        done = true;
        if (argc < 2) { out("Err: usage: set <what> <value>"); }
        const char* s = argv[1];
        if      (!strcmp(s, "name"))                set_name(argc, argv);
        else if (!strcmp(s, "lat"))                 set_coord(argc, argv, 1);
        else if (!strcmp(s, "lon"))                 set_coord(argc, argv, 0);
        else if (!strcmp(s, "radio"))               set_radio(argc, argv);
        else if (!strcmp(s, "freq"))                set_freq(argc, argv);
        else if (!strcmp(s, "tx"))                  set_tx(argc, argv);
        else if (!strcmp(s, "flood.advert.interval")) set_flood_adv(argc, argv);
        else if (!strcmp(s, "advert.interval"))       set_zero_adv(argc, argv);
        else if (!strcmp(s, "repeat"))              set_repeat(argc, argv);
        else if (!strcmp(s, "loop.detect"))         set_loop_detect(argc, argv);
        else if (!strcmp(s, "txdelay"))             set_txdelay(argc, argv, false);
        else if (!strcmp(s, "direct.txdelay"))      set_txdelay(argc, argv, true);
        else if (!strcmp(s, "dutycycle"))           set_dutycycle(argc, argv);
        else if (!strcmp(s, "af"))                  set_af(argc, argv);
        else if (!strcmp(s, "flood.max"))           set_flood_max(argc, argv);
        else out("Err: unknown (name|lat|lon|radio|freq|tx|repeat|loop.detect|txdelay|direct.txdelay|dutycycle|flood.max|flood.advert.interval|advert.interval)");
        done = true;
    }

    if (!done) {
        out("Err: unknown command (ver|board|role|reboot|erase|clock|time|advert|advert.zerohop|neighbors|discover.neighbors|stats-core|stats-packets|log|get|set)");
        done = true;
    }
    #undef FINISH
}
