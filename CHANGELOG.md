# Changelog

All notable changes to SPECTER firmware are documented here.

## [1.15.3]

### Analog calibration CLI (VREFINT + temperature offset)

Per-chip analog calibration, persisted in a new flash record (words [18..19] of
the prefs page, independent of the NodeConfig/AdminConfig records — old devices
without it fall back to defaults, so it's backward-compatible):

- **`battery.charged [V]`** — with a fully-charged pack, rescales the internal
  VREFINT so the current VDDA reading maps to (V − Schottky drop), then persists
  it. `V` is the full-charge voltage (default `4.2`); pass the real value if your
  pack tops out lower (e.g. `battery.charged 4.15`). This zeroes the `battery`
  reading against a known reference (replaces the old board-hardcoded
  `VREFINT_MV`). Note: VREFINT also scales the temp sensor (via VDDA), so a
  correct calibration improves the temp reading too — calibrate with a *known*
  pack voltage, not a guess, or both readings shift together.
- **`get battery.vref` / `set battery.vref <mV>`** — read/set the internal
  VREFINT (1000–1400 mV) directly.
- **`get temp.offset` / `set temp.offset <-12..12>`** — read/set an additive
  whole-°C offset applied to the internal temperature sensor, to zero it against
  a reference thermometer.
- The calibration **writes** are ADMIN-gated (admin or local serial only); the
  **gets** are guest-readable.

Also:

- **`dfu`** now prints a hint to close the serial monitor and wait ~25–30 s for
  the CH340 to re-enumerate before uploading (the USB link drops on the reset).

## [1.15.2]

### Telemetry request (REQ `0x03`) + `temp` command

- **Telemetry request** (`REQ_TYPE_GET_TELEMETRY_DATA` = `0x03`): the app's
  *Telemetry* button now gets a proper response. Reply = `[sender_ts(4)][CayenneLPP]`
  carrying two points on channel 1 (`TELEM_CHANNEL_SELF`), matching the official
  firmware's `addVoltage` + `addTemperature`:
  - **voltage** — LPP type `0x74` (electroniccats/CayenneLPP 1.6.1), 2 bytes
    big-endian, `mV / 10` (0.01 V units).
  - **temperature** — LPP type `0x67`, 2 bytes big-endian, `°C × 10` (0.1 °C units).
  Both are *base* telemetry, so they're **guest-readable** (like `get stats`).
  Note: electroniccats encodes these as type `0x74`/`0x67` (0.01 V / 0.1 °C),
  which differs from the myDevices LPP standard the app *also* understands —
  SPECTER follows electroniccats because that's what the official firmware emits.
- **`temp`** command (USB + mesh CLI): prints the MCU die temperature in whole
  °C (e.g. `25°C`), from the internal ADC ch16 sensor (datasheet formula).
  The ADC reading is scaled against the **clean VDDA** supply — the Schottky
  drop is kept in a separate `measure_battery_mv()`, so it can't corrupt the
  temperature. Integer formatting (newlib-nano has no float printf).

Also included: battery/supply voltage via
the internal VREFINT (`battery` command + `RepeaterStats.batt_milli_volts`).

## [1.15.1]

### STOP mode + serial auto-sleep removed

The STOP-mode deep sleep and the PA9/PA10 serial auto-sleep (60 s idle timeout)
features are **removed**. The idle path now always uses `__WFI()` (8 MHz SLEEP).

Reason: the DIO1 (PC15, EXTI 15) and RTC-alarm (EXTI 17) wakeup sources did not
reliably pull the core out of STOP. The board slept through incoming LoRa packets
(remote login failed) and the periodic RTC alarm failed to service the 8 s IWDG,
so the watchdog reset the board (boot `RESET:` line reported `IWDG`). The ~1.5 mA
saved was not worth the complexity and the remote-session breakage.

Kept: the 8 MHz HSI downclock (-8 mA, below) - that was the real win - and the
boot `RESET:` reason log (useful for diagnosing future resets). Also corrected
`docs/hardware.md` DIO1 to PC15 (was PA15).

### Downclocked to 8 MHz HSI (PLL completely powered down)
User reported idle current to drop to 8 mA

### RX gain feature
- **`get/set radio.rxgain on|off`** — RX front-end gain (SX1262 reg `0x08AC`):
  **off** = power-saving (reg `0x94`, less sensitive, lower current — the
  official firmware's default), **on** = boosted (reg `0x96`, more sensitive).
  SPECTER previously never touched the register, so it ran on the chip's reset
  default rather than the official's explicit power-saving setting. It is now
  applied at radio init (persisted in the node-config record — the new field
  lives in the record's former trailing pad, so the on-flash size/CRC are
  unchanged and existing devices default to **off**), and the command applies it
  **live** (no reboot).
  **Measured:** `on` draws ~0.5 mA more than `off` through the battery (16.1 vs
  15.6 mA) — the boosted LNA state's own current, with no change in RX quality
  or spurious readouts. That ruled out the "gain latches background noise and
  wakes the CPU" theory as the cause of the day/night current swing. The command
  is kept regardless: it's a standard radio control the official firmware also
  exposes, and `off` is the free lower-current default.

## [1.15.0]

Adds **remote mesh administration**: SPECTER can now be administered over the
LoRa mesh by an official MeshCore companion radio, byte-for-byte compatible
with the companion firmware's peer-data protocol — including the app's
**Repeater Status** (`get stats`), so the whole companion UI works over the
mesh. USB remains the primary channel; this is field access.

### Added
- **Mesh admin protocol** (`src/main.cpp` + `src/acl.cpp`): a companion
  logs in with an **ANON_REQ** (ephemeral Ed25519 key + password, encrypted
  with `X25519(their key, our pubkey)`), and then runs **CLI commands** as
  TXT_MSG (`TXT_TYPE_CLI_COMMAND`) / replies as TXT_MSG (`TXT_TYPE_CLI_DATA`),
  all AES-128-ECB (zero-padded) + 2-byte truncated HMAC-SHA256. Replies are
  *flooded* but stay confidential — only the client and SPECTER derive the
  session secret.
- **`src/meshcrypto/`** — byte-exact port of the official wire crypto:
  `key_exchange.c` + `fe.c` (X25519, RFC 7748 with Ed25519-seed clamping) and
  a C++ wrapper (`meshcrypto.cpp`) over the rweather/Crypto AES128/SHA256.
  Verified on host against a verbatim copy of MeshCore `Utils.cpp` and a full
  client/server login + CLI round-trip.
- **Session table** (`acl.cpp`, in-memory, 4 slots): keyed by the client's
  ephemeral pubkey; holds the per-session shared secret + permission +
  replay-guard timestamp. A reboot ends all sessions.
- **CLI**: `set/get admin.password`, `set/get guest.password`,
  `set/get admin.timeout`, `acl` (list active sessions), and the official
  `password <pwd>` shortcut (sets the admin password and echoes it back).
- **`ADMIN_PASSWORD` build flag**: bakes a default admin password into the
  firmware (like `NODE_LAT_I`/`NODE_LON_I`). Seeded at first boot; a CLI
  `set admin.password` (or clearing it) overrides it in flash, so admin can
  still be fully disabled over the mesh.
- **`get stats` (REQ 0x01) → Repeater Status**: the app's *Repeater Status*
  button now works. SPECTER answers the `REQ_TYPE_GET_STATUS` request with a
  byte-exact `RepeaterStats` (56 B, little-endian — matching the official
  `simple_repeater`): uptime, cumulative TX/RX airtime, last RX RSSI/SNR,
  **noise floor**, packet counts (recv/sent, flood/direct), direct/flood dups,
  recv errors. Guest-accessible (read-only). Backed by live counters in the
  RX/TX paths (`stat_last_rssi`, `stat_noise_floor`, `stat_tx/rx_air_ms`,
  `stat_sent/recv_flood/direct`, `stat_direct/flood_dups`, `stat_recv_errors`).
  The **noise floor** is the ambient instantaneous RSSI (`getRSSI(false)` /
  `GET_RSSI_INST`) while idle in RX — average of 10 quiet samples (below floor
  + 14 dB, the official's `SAMPLING_THRESHOLD`), clamped to −120 dBi, sampled
  every 100 ms; self-correcting, ignores transient signals.
- **Permission model**: admin password → full (incl. `set`/`reboot`/`erase`/`dfu`);
  guest password → read-only (`get`, `stats-*`, `neighbors`, `log`, `acl`, …).
  Idle sessions expire after `admin.timeout` seconds (default 600; 0 = never).
- **Boot LED signature** — 5 quick flashes the instant the app starts (before
  radio init), so a successful app boot is easy to distinguish from the ROM
  bootloader's solid LED (see [flashing.md](docs/flashing.md)).
- **Split logging** — `slog` (remote `log` ring + USB) carries terse operational
  events (login, per-RX signal level, errors, `ALIVE`, adverts); a new `dlog`
  (USB only) carries the verbose per-packet detail (fingerprint traces, command
  hex dump, relay, drop reasons, `ANON`/`TXM`/`REQ`). The remote `log` is now a
  clean, terse ring; the debugging detail stays on the USB console.
- **`log start` / `log stop`** — the per-packet RX signal line is now opt-in
  to the remote ring (matching the official's `log start`/`log stop` `_logging`
  gate). Off by default so a busy mesh doesn't flood the ring; `log start`
  begins capturing RX RSSI/SNR/len into the ring, `log stop` ends it. The line
  still prints to the USB console either way (live debug).

### Fixed
- **X25519 scalar (critical interop bug)**: the key exchange must use the
  Ed25519 scalar `a = SHA512(seed)[0:32]` clamped — **not** the raw seed. The
  old code passed the raw seed, which would never agree with a real companion.
  Now `own_x25519_key` is derived correctly (and the SHA-512 `finalize(len)`
  bug that zeroed it is fixed). Caught by the host round-trip test.
- **Command flag type**: the app sends CLI commands as `TXT_TYPE_CLI_DATA (1)`,
  not just `TXT_TYPE_CLI_COMMAND (3)`. The server now accepts **both** as
  runnable commands (the official `CLI_DATA` type is documented as "a CLI
  command -or- reply"). Genuine replies are addressed to the client, never to
  us, so accepting `1` can't mis-fire. Caught by the field diagnostic log.
- **Transaction-ID prefix**: the app prefixes each command with a short
  session/transaction ID (`<sess>|<command>`) to correlate replies. SPECTER now
  strips the prefix, runs the command, and echoes it back in the reply
  (`<sess>|<reply>`). Without this every app command came back "unknown".
- **Unknown-command reply names the command**: the mesh reply (and log) now
  shows the actual unrecognized command (`Err: unknown command '<cmd>'` / `get` /
  `set`), instead of a generic list, so missing app features are identifiable.
- **TXT_MSG not null-terminated (critical interop bug)**: the sender sizes the
  datagram to exactly `5 + text_len` and omits the null terminator, so when
  `5 + text_len` is a 16-byte multiple there is **no zero padding** and the
  plaintext isn't null-terminated. `strnlen` then ran into stale buffer bytes,
  corrupting any such command (`get name` → `get namet` / `get namert.interval`,
  intermittently, depending on what was in the buffer). The decrypt buffer is
  now pre-cleared to zero so the text boundary is always a real null.
- **Full-message diagnostic**: each incoming TXT_MSG is hex-dumped (`TXM … hex:`
  + `sess='…' cmd='…'`), which is how the bug above was caught. (Now USB-only via
  `dlog`, so the remote `log` ring stays clean.)
- **Duplicate replies** (two gaps, both fixed):
  1. *Reply side* — `send_admin_reply` now records its own fingerprint on TX
     (matching the official's `markSeen`, "in case it is rebroadcast back to us"),
     so a re-relayed copy of our reply isn't relayed again.
  2. *Command side (the actual cause)* — the addressed-to-us admin block
     processed a TXT_MSG and `return false` **without recording its
     fingerprint** (it returns before the flood-relay dedup). So a re-relayed
     copy of the *command* that looped back through another repeater was
     reprocessed, re-sending the reply. The admin block now records + drops
     duplicates (`Drop: dedup admin`).
- **Get Position timeout (p=8 relayed)**: the app's `PAYLOAD_TYPE_PATH` (0x08)
  returned-path packet, when addressed to us, was treated as an ordinary flood
  and relayed. That relay's 300–800 ms CSMA backoff blocked the main loop right
  as the app's `get lon` arrived, so `get lon` timed out (standalone `get lon`
  worked fine). Now a p=8 addressed to us is dropped (a returned-path request is
  irrelevant to a repeater); a p=8 *not* addressed to us is still relayed.

### Changed
- **Blank-password guest login** — with no guest password set, a blank password
  is accepted as a guest (read-only) login, so the app's guest / Repeater-Status
  path works with zero setup. A set guest password still requires the real
  password.
- **`set guest.password` / `set admin.password` clear on bare command** — running
  the setter with no value clears the password (matching the official's "empty
  value clears" semantics).
- **Log ring 32 → 64 lines** (3 KB) and **mesh reply cap 150 → 176 B** (182 B
  packet). The official caps `MAX_PACKET_PAYLOAD` at 184 B, so 176 B is the
  one-packet interop ceiling — a larger reply is silently dropped by the
  companion.
- **LED heartbeat vs. `ALIVE`** — the LED still blinks every 10 s; the
  `ALIVE up=… rx=… relay=… drop=…` log line now appears every 60 s (keeps the
  remote ring from getting a line every 10 s).
- **Flood-relay backoff + per-packet airtime** — `send_pkt`'s fixed 300–800 ms
  "CSMA" delay (`delay(300 + millis()%500)`) is gone: it was deterministic (no
  RTC, so `millis()` ticks in lockstep across nodes → identical backoffs), hit
  *every* TX, and ignored packet size. On-air time is now computed per-packet
  from `radio.getTimeOnAir(len)` (live SF/BW/CR) — feeding the retransmit
  delay, the token-bucket duty gate, and per-packet RX air-time stats. The
  retransmit delay is applied **only to flood relays** (collision avoidance):
  a per-chip random 0..~2× airtime (LCG seeded from the 96-bit STM32 UID).
  Originals (adverts/admin replies) and direct retransmits / PING responses are
  undelayed (the official's `getDirectRetransmitDelay = 0`).
- **`neighbors` now matches the official wire format** — the app's Neighbours
  map was blank because SPECTER's reply used a non-standard layout (2-byte
  pubkey prefix, absolute timestamp) and returned 0 bytes when empty. Now it
  emits `<pubkey[4B-hex]>:<secs_ago>:<snr*4>` newest-first with `-none-` for an
  empty cache, and the cache excludes our own ADVERT (matches the official's
  `self_id.matches()` guard). Verified: the app now plots SPECTER and a
  stock-firmware repeater on the map.

### Notes (follow-ups)
- The periodic **ADVERT** TX has the same not-marked-as-seen gap; it's less
  visible (adverts are periodic) and left for a follow-up so this release stays
  focused on the reported duplicate-reply symptom.

### Notes
- Admin/guest credentials are stored in a **separate, versioned flash record**
  (words 20–35, magic `AdmN` + CRC16); the node-config record is byte-identical,
  so there is no migration and an old-layout `erase` still works.
- Version kept below 1.16.0 so `config.meshcore.io` does **not** expose the
  `flood.max.unscoped` / `flood.max.advert` variables we don't implement; all
  features up to `loop.detect` (1.14.0) still work.

## [1.14.0]

Makes SPECTER administrable by the official **config.meshcore.io** web
client (and the flasher serial console) over USB. Verified live against the
real app's parser.

### Added
- **`  -> > <value>` `get` reply format** — `config.meshcore.io`'s
  `parseVariableResponse()` requires every `get` reply to start with `> `
  (it does `response.startsWith("> ")` then strips it). All `get` handlers
  now emit `  -> > <value>` (via new `getv()` / `getv_num()`), matching the
  official firmware. Without it the app marked every variable "unsupported"
  and silently failed to save anything.
- **`get af` / `set af`** — bridge for the app's airtime-factor variable
  (`duty% = 100/(af+1)`) to our stored `dutycycle`, so the airtime slider
  round-trips. (`dutycycle` still works directly.)
- **Degraded mode on radio-init failure** — a bad radio setting no longer
  halts the device and bricks the CLI. The radio-init failure now leaves the
  CLI alive, logs it, and retries `radio.init()` every 3 s (triple-blink LED
  while down). Fix the setting over serial and the radio recovers without a
  reflash.

### Improved
- **Token-bucket RF duty cycle** (was a fixed per-packet silence). Matches the
  official MeshCore `Dispatcher`: a budget of airtime (ms) refills continuously
  at the `dutycycle` rate over a 1-hour window (3 600 000 ms). Bursts up to the
  bucket are allowed; each TX consumes its *measured* airtime and is gated until
  enough budget is available (the wait is budget-deficit based, not a fixed
  `airtime × (100/duty−1)`). Idle time tops the bucket up, so a burst after idle
  is fine and the average airtime over the window stays ≤ duty. Integer
  fixed-point (no float); the remaining budget is visible via `stats-core`
  (`budget=<ms>`). The bucket starts full at boot.

### Fixed
- **`set radio` rejected every valid LoRa bandwidth** — the BW check bounds
  were `7–50 kHz` (micro `7000000–50000000`), so real bandwidths (62.5/125/
  250/500 kHz) were refused and only invalid ones passed. Now `7.8–500 kHz`
  (SX1262 range). Frequency bounds corrected to the SX1262's `150–960 MHz`
  (was 300–2500). SF 5–12 and CR 5–8 unchanged.

### Changed
- Version string `1.14.0`. This is what `config.meshcore.io` uses for
  feature detection (`varMinVersion`): 1.14.0 exposes `loop.detect`. The
  number only drives `supportsVar`, nothing else.

---

## [1.4.0]

### Added
- **Interactive shell UX**: `> ` prompt after boot and after every command
- **`log` command** — print the 20 most recent diagnostic lines;
  `log clear` empties the buffer
- 32-line (48 char) diagnostic ring buffer (RX, relay, drop, ADVERT,
  heartbeat, radio errors)

### Changed
- Serial console is **silent by default**: no continuous log stream.
  Only the boot banner (3 lines), command replies, and the prompt are
  printed. All runtime diagnostics go to the ring buffer instead —
  read them on demand with `log`.
- CLI accepts **CR, LF, and CRLF** line endings (any terminal), and
  backspace now erases the last character of the pending line

---

## [1.3.0]

### Changed
- Firmware version string `1.3.0` (`ver` command)

### Documentation
- `docs/cli.md`: new **§3 Using the CLI** — finding the serial port
  (macOS/Linux), port-contention check (PommeCore), connection methods
  (miniterm / screen / pio monitor), full tested command reference table,
  settings storage, button-less flashing via `dfu`
- `README.md`: Serial Console & CLI section (port discovery, connection,
  CLI quick tour), updated memory stats (61 KB / 46.8 %), project
  structure with `cli.cpp` / `prefs.cpp` / `config.h`, troubleshooting
  entries (port contention, newlib-nano float printf)
- `docs/development-notes.md`: newlib-nano float printf no-op, USB serial
  port contention
- `docs/hardware.md`, `docs/flashing.md`: notes on optional `dfu`
  bootloader entry

---

## [1.2.1]

### Added
- **USB-serial CLI** (MeshCore command subset for the repeater role):
  `ver`, `board`, `role`, `public.key`, `clock`, `time`, `name`, `lat`,
  `lon`, `radio`, `freq`, `tx`, `flood.advert.interval`, `advert.interval`,
  `repeat`, `loop.detect`, `txdelay`, `direct.txdelay`, `dutycycle`,
  `flood.max`, `advert`, `advert.zerohop`, `neighbors`, `discover.neighbors`,
  `stats-core`, `stats-packets`, `reboot`, `erase`
  (full reference: `docs/cli.md`)
- **Persistent settings** in reserved flash page 0x0801F000 (magic + CRC16);
  build flags become defaults, stored settings win; `erase` = factory reset
- **EU duty-cycle enforcement**: configurable `dutycycle` (default 10 %),
  enforced as post-TX silence scaled to estimated on-air time
- **8-slot neighbor cache** backing `neighbors` (`pubkey:ts:snr*4` format)
- **Discovery burst**: `advert.zerohop` / `discover.neighbors` send 5×1 s
  zero-hop adverts
- **Runtime gates**: `set repeat off` disables relaying, `flood.max` hop
  limit, `loop.detect` levels
- **`time <epoch>`** re-syncs the advert clock (compile-time base previously
  drifted, risking replay rejection by companions)
- **`dfu` CLI command** (compile-time optional, `-D DFU_BOOT0_PIN` /
  `-D DFU_RST_PIN`): programmatic entry into the ROM serial bootloader for
  button-less flashing

### Changed
- Radio parameters, node name, location and advert intervals are now
  runtime-configurable (were compile-time only); EU868 defaults unchanged
  (869.618 MHz, SF8, BW 62.5 kHz, CR 8, 14 dBm)
- All CLI float formatting/parsing rewritten as integer fixed point
  (newlib-nano has no float `printf`/`scanf` — see development notes)
- Memory: 61.3 KB flash (46.8 %), 4.6 KB RAM (22.3 %)

### Fixed
- `get radio`/`get lat`/`get lon`/`get freq`/`get txdelay` printed empty
  values (newlib-nano float printf stub)
- `set name` / `set radio` failed with "Err: parse" (tokenizer destroyed
  the line buffer before re-scan)

### Tested on hardware
- Full CLI smoke test (all reads/writes, validation errors, reboot,
  flash persistence) — passing; see `docs/cli.md` §5 progress log

---

## [1.1.0]

### Added
- **Ed25519 cryptographic signing** of ADVERT packets using rweather/Crypto library
- Deterministic key derivation from STM32 UID via SHA256
- Non-blocking receive mode using `startReceive()` + IRQ flag polling
- Comprehensive documentation in `docs/development-notes.md`
- Critical bootloader baud rate documentation (57600, not 115200)

### Changed
- Node identity now based on Ed25519 public key (not pseudo-random)
- Node name: `SPECTER-XXXX` derived from UID (e.g., `SPECTER-1811`)
- Memory usage: 51.7 KB flash (39.5%), 3.1 KB RAM (15.3%)
- Updated `flash.sh` and `platformio.ini` with correct 57600 baud rate

### Fixed
- **CRITICAL**: MeshCore companions now recognize the repeater (Ed25519 validation)
- **CRITICAL**: Receive no longer blocks forever (DIO1=RADIOLIB_NC polling fix)
- Radio configuration matches Heltec companion: 869.618 MHz, SF8, BW62.5 kHz

### Documentation
- Added `docs/development-notes.md`, critical lessons learned
- Updated `docs/protocol.md`, Ed25519 signature format
- Updated `docs/flashing.md`, 57600 baud requirement
- Updated `README.md`, dependencies, troubleshooting, Ed25519 overview

---

## [1.0.0] - Initial Release

### Added
- Basic MeshCore flood repeater implementation
- SX1262 driver via RadioLib 7.6.0
- STM32F103C8T6 support with CH340 USB-serial
- Auto-generated node name from UID
- Dedup cache (64 slots, circular buffer)
- ADVERT generation (every 12 hours + initial at boot)
- Flood relay with hop limiting and loop prevention

### Known Issues (Fixed in v1.1.0)
- ADVERT signature field was all zeros → companions ignored the node
- Blocking `radio.receive()` with DIO1=RADIOLIB_NC → no packets received
- Documentation stated 115200 baud → bootloader actually requires 57600

---

## Development Timeline

### Phase 1: Initial Hardware Bringup
- Pin mapping discovery from original DX-LR30 AT firmware
- RadioLib integration with SPI1 + RF switch pins
- Serial console via USART1 (ENABLE_HWSERIAL1 flag required)

### Phase 2: MeshCore Protocol Implementation
- Packet header parsing (route_type, payload_type, hop_count)
- Path hash tracking for loop prevention
- Dedup cache to avoid relay storms
- ADVERT payload format (flags, node name)

### Phase 4: Critical Fixes
- **Day 1**: "0 repeaters" in app → suspected radio params
- **Day 2**: Radio params matched, still not visible → suspected signature validation
- **Day 3**: Implemented Ed25519 signing → **REPEATER APPEARED!**

### Phase 5: Documentation & Polish
- Created comprehensive docs/ directory
- Documented all critical discoveries in development-notes.md
- Updated README with troubleshooting and Ed25519 overview
- Added warnings about baud rate and polling mode throughout codebase

---

## Versioning

This project follows [Semantic Versioning](https://semver.org/):
- MAJOR version: Breaking protocol or hardware changes
- MINOR version: New features, backward-compatible
- PATCH version: Bug fixes, documentation updates

---

## Contributing

See `docs/development-notes.md` for critical implementation details.

When reporting issues, include:
- Firmware version (check serial output on boot)
- Companion device and firmware version
- Radio parameters in use
- Serial monitor output showing the issue

---

## License

MIT License, see LICENSE file for details.
