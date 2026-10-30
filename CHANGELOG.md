# Changelog

All notable changes to SPECTER firmware are documented here.

## [1.14.0] - Current

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

### Notes
- **CH340 USB quirk:** after a CLI `reboot`, the Mac-side USB-serial link
  drops and needs a cable power-cycle to re-establish. The device reboots
  fine underneath and all settings persist across the reboot (verified).

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

### Phase 3: Radio Parameter Tuning
- Discovered companion was configured at 869.618 MHz (not default 869.525)
- Confirmed SF=8, BW=62.5 kHz, CR=4/5, SyncWord=0x12

### Phase 4: Critical Fixes
- **Day 1**: "0 repeaters" in app → suspected radio params
- **Day 2**: Radio params matched, still not visible → suspected signature validation
- **Day 3**: Implemented Ed25519 signing → **REPEATER APPEARED!**
- **Day 4**: Discovered receive blocking issue → switched to polling mode
- **Day 5**: Flash failures at 115200 → discovered 57600 baud requirement

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
- Whether `stm32flash -b 57600` succeeded or failed

---

## License

MIT License, see LICENSE file for details.
