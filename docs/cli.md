# SPECTER CLI — Command Plan & Progress

Goal: implement a subset of the standard MeshCore CLI (see `CLI.pdf` /
`docs/cli.md` reference) suitable for this device's **Router (repeater) role**.

Hardware constraints that shape the subset:
- STM32F103CB + SX1262 (DX-LR30), ~20 KB RAM, 128 KB flash
- **No user buttons** — USB serial (CH340, 115200 baud) is the *only*
  admin channel
- No GPS, no sensors, no bridge, no Ethernet, no battery, no FEM
- No remote (LoRa) admin channel → "Serial Only" commands are the *only*
  ones we need to support; commands marked "Serial Only: No" in the
  reference doc must additionally work over LoRa and are lower priority

## 1. Required command list (prioritized)

### Tier 1 — essential (user request: radio, advert intervals, version, name, location)

| # | Command | Type | Notes / EU default |
|---|---------|------|--------------------|
| 1 | `ver` | get | Firmware version + build date (`FW_VERSION` + `__DATE__`) |
| 2 | `get name` / `set name <name>` | get/set | Max 23 bytes with location. Default: `SPECTER-XXXX` (auto from STM32 UID) or `NODE_NAME_STR` build flag. Persisted to flash |
| 3 | `get lat` / `set lat <deg>` | get/set | Degrees, 5 decimals. Default from `NODE_LAT_I` build flag (currently 48.961945) |
| 4 | `get lon` / `set lon <deg>` | get/set | Degrees. Default from `NODE_LON_I` build flag (currently 9.129164) |
| 5 | `get radio` / `set radio <freq>,<bw>,<sf>,<cr>` | get/set | EU868 default: `869.618,62.5,8,5`. Requires reboot to apply → reply must say so |
| 6 | `get flood.advert.interval` / `set <hours>` | get/set | 3–168 h. Default **47** (MeshCore repeater default) |
| 7 | `get advert.interval` / `set <minutes>` | get/set | 60–240 min, rounded down to 2. Zero-hop adverts. Default **60** (current firmware hardcodes 2 min; 60 is the sane steady-state EU value, 0 = disabled) |
| 8 | `reboot` | op | No reply, per reference doc. Handy since USB is the only admin path |

### Tier 2 — important for a repeater

| # | Command | Type | Notes / default |
|---|---------|------|-----------------|
| 9 | `get public.key` | get | 64-hex-char Ed25519 pubkey (already computed) |
| 10 | `board` | get | Hardware name → `DX-LR30` |
| 11 | `get role` | get | Static → `repeater` |
| 12 | `get tx` / `set tx <dbm>` | get/set | 1–22 dBm (SX1262). Default **14** (safe under EU 27 dBm EIRP incl. module gain). Applied at next radio init/reboot |
| 13 | `get freq` / `set freq <MHz>` | get/set | Shortcut for single-frequency change. Default **869.618** (EU868). Reboot to apply |
| 14 | `advert` | op | Trigger a flood ADVERT now (already have `build_advert()`) |
| 15 | `advert.zerohop` | op | Trigger a zero-hop ADVERT now |
| 16 | `neighbors` | get | List up to 8 most recent adverts: `{pubkey-prefix}:{timestamp}:{snr*4}` (needs a small neighbor cache — new, ~8×40 B) |
| 17 | `discover.neighbors` | op | Trigger zero-hop discovery burst |
| 18 | `stats-core` | get (serial) | Uptime, advert counters, relay/drop counters (already tracked) |
| 19 | `stats-packets` | get (serial) | rx / relay / drop counters (already tracked) |
| 20 | `clock` / `time <epoch>` / `clock sync` | op | Advert timestamps are compile-time base + `millis()` — drifts. `time` lets user re-sync the base; required because advert replay protection rejects `|now-ts| > 3600 s` |

### Tier 3 — routing / regulatory tuning

| # | Command | Type | Notes / EU default |
|---|---------|------|--------------------|
| 21 | `get repeat` / `set repeat on\|off` | get/set | Relay enable flag. Default **on** |
| 22 | `get loop.detect` / `set off\|minimal\|moderate\|strict` | get/set | Default **minimal** (recommended over reference `off` for EU dense meshes; current firmware already drops "our hash in path") |
| 23 | `get txdelay` / `set <0-2>` | get/set | Flood retransmit delay factor. Default **0.5** (current 300–800 ms backoff matches) |
| 24 | `get direct.txdelay` / `set <0-2>` | get/set | Default **0.3** (repeater) |
| 25 | `get dutycycle` / `set <1-100>` | get/set | EU 868 MHz → default **10** (%), stricter than reference 50 % |
| 26 | `get flood.max` / `set <0-64>` | get/set | Default **64** |
| 27 | `erase` | op (serial) | Factory reset: clear preferences page + reboot. Destructive — but with USB as the only admin path, this is the recovery mechanism |

### Explicitly NOT implemented (no hardware / not applicable to this role)

- `gps *` (no GPS module), `sensor *`, `bridge *`, `eth.*` (no modules)
- `pwrmgt.*` (mains/USB powered, no power management)
- `password`, `guest.password`, `setperm`, `get acl`, `allow.read.only`
  (no remote admin channel; LoRa-admin ACL is meaningless)
- `region *` (this build has no region/transport-code support)
- `start ota`, `clkreboot` (no OTA mechanism yet)
- `radio.rxgain`, `radio.fem.*` (no FEM on DX-LR30)
- `path.hash.mode`, `flood.max.advert`, `flood.max.unscoped`, `rxdelay`,
  `multi.acks`, `cad`, `agc.reset.interval`, `int.thresh`, `adc.multiplier`
  (defer; hash size is fixed at 1 byte in current protocol impl)
- `neighbor.remove` (defer with `neighbors` cache work — trivial add later)

## 2. EU868 default settings summary

| Parameter | Default | Rationale |
|-----------|---------|-----------|
| Frequency | 869.618 MHz | MeshCore standard EU868 default, in-band (863–870 MHz) |
| Bandwidth | 62.5 kHz | MeshCore default, good range/throughput tradeoff |
| Spreading factor | 8 | MeshCore default |
| Coding rate | 4/8 | MeshCore default CR 5 ↔ verify mapping to RadioLib (see Progress) |
| TX power | 14 dBm | Conservative; keeps EIRP well under EU 27 dBm |
| Flood advert interval | 47 h | MeshCore repeater default |
| Zero-hop advert interval | 60 min | MeshCore minimum (60); 0 disables |
| Duty cycle | 10 % | EU 868 MHz aggregate limit (1 % per channel) |
| Name | `SPECTER-XXXX` / build flag | Auto from STM32 UID |
| Lat / Lon | build flags (48.961945, 9.129164) | `NODE_LAT_I` / `NODE_LON_I` |
| Loop detect | minimal | Safe default for dense EU meshes |
| Flood max hops | 64 | MeshCore default |

## 3. Using the CLI (operating guide)

The CLI is a line-based command interface on **USART1 → CH340 USB-serial**,
**115200 8N1**. There are no buttons on the device — USB is the only admin
channel. Commands are case-sensitive lowercase, sent with a trailing newline
(Enter). Replies: bare value, `OK`, `OK, reboot to apply`, or `Err: ...`.

### 3.0 Which client to use (repeater vs. companion)

The official MeshCore firmware (github.com/meshcore-dev/MeshCore) has **two
distinct USB serial protocols**, one per device role:

| | **Repeater** (SPECTER's role) | **Companion Radio / Sensor** |
|---|---|---|
| Reference source | `examples/simple_repeater/main.cpp` (built into all `*_repeater` production firmwares) | `examples/companion_radio/main.cpp` + `helpers/ArduinoSerialInterface.cpp` |
| USB protocol | **Plain-text line CLI** (this document) | **Framed binary**: `[0x3C/0x3E][len:2 LE][data]` |
| Intended client | A serial terminal (miniterm / screen / Putty) | The phone apps (PommeCore, meshcore.js, meshcore_py) |
| Auth over USB | none | `SendLogin(pubkey, password)` frame |

**The standard apps that administer a *repeater* over USB** are the web
clients **config.meshcore.io** and the **flasher.meshcore.io serial
console** (the phone apps — PommeCore / meshcore.js — are *companion*
clients and pair with Companion Radios via the framed protocol above; they
cannot target a repeater). Those web clients speak plain text, but with a
specific wire format that SPECTER must match byte-for-byte:

| Wire detail | Official repeater (`simple_repeater/main.cpp`) | `config.meshcore.io` parser (`lib/serial-cli.js`) |
|---|---|---|
| Command | `command\r` (CR terminates) | sends `command + '\r'` |
| Input echo | `Serial.print(c)` for every char | tolerates, strips the echoed `command\r\n` |
| Reply marker | `Serial.print("  -> "); Serial.println(reply)` | **`readBuffer.indexOf("  -> ")` — required** |
| Reply terminator | CRLF (`\r\n`) | **`indexOf('\r\n', payloadStart)` — required** |
| `get` value prefix | `get name` → reply `"> MyNode"` | **`parseVariableResponse`: `response.startsWith("> ")` then strips it — required** |
| Multi-line `log` | lines … then `   EOF` on its own line | collects until quiet (`idleMs`) |

So there were **three** things the standard app needed and SPECTER lacked:
**(1)** no `  -> ` reply marker, **(2)** LF instead of CRLF line ends, and
**(3)** — the one that actually broke *saving* — `get` replies had no `> `
value prefix, so `parseVariableResponse()` returned null for every variable,
the app marked them all "unsupported", and `setData()` skipped them all
(silently re-reading the old values and reporting success). Fixed in
**v1.14.0**: SPECTER now echoes input, answers `  -> <reply>\r\n`, emits
`get` replies as `  -> > <value>\r\n` (via `getv()`/`getv_num()`), and
bridges the app's `af` airtime variable to `dutycycle`.

> Verified working: `config.meshcore.io` connects, reads `ver`, and
> `get`/`set` of name, radio, tx, lat/lon, airtime (`af`) all parse and
> **persist across a reboot** (replayed the real `serial-cli.js` parser
> against the live device).

(Companion↔repeater configuration *over the LoRa mesh* — signed LOGIN/
REQUEST/RESPONSE packets — is a separate protocol that SPECTER deliberately
does not implement; see Progress log 2026-07-15 (4).)

### 3.1 Finding the device

**macOS** (CH340 driver must be installed, e.g. `brew install ch341`
if the port never appears):

```bash
ls /dev/cu.usbserial-*        # e.g. /dev/cu.usbserial-10
```

**Linux:**

```bash
ls /dev/ttyUSB*               # e.g. /dev/ttyUSB0
# or: dmesg | tail  after plugging in (CH340 = id 1a86:7523)
```

**⚠️ Check the port is not held by another app first.** The MeshCore
companion (PommeCore) opens the port and silently consumes all serial
data — the device looks alive (LED blinks, relays packets) but nothing
reaches your terminal:

```bash
lsof /dev/cu.usbserial-10     # macOS
# fuser /dev/ttyUSB0          # Linux
```

Quit any listed app before connecting.

### 3.2 Connecting

```bash
# Recommended: pyserial miniterm (sends clean LF line endings)
python3 -m serial.tools.miniterm /dev/cu.usbserial-10 115200 --eol LF
#   (install: python3 -m pip install --user pyserial)

# Or: screen
screen /dev/cu.usbserial-10 115200          # exit: Ctrl-A then K, y

# Or: PlatformIO monitor
~/.platformio/penv/bin/pio device monitor -p /dev/cu.usbserial-10 -b 115200
```

After boot you get a 3-line banner and an interactive prompt. The console is
**silent otherwise** — all runtime diagnostics (heartbeats, RX, relay, drop,
ADVERT lines) are captured in a 32-line ring buffer and only printed when you
ask for them:

```
=== SPECTER MeshCore Repeater v1.4.0 [DE.BW.LB-Bi-Bi.STM32] ===
Hash: 0x36
Radio init... OK
> ver
1.4.0 (Oct 26 2026)
> log
RX RSSI=-97 SNR=11 len=53
Drop: our hash in path
ADVERT initial
ALIVE uptime=30s ts=1793052690 rx=1 relay=0 drop=1
> log clear
OK
>
```

### 3.3 Command reference (as implemented, v1.4.0)

| Command | Reply | Notes |
|---|---|---|
| `ver` | `1.4.0 (Oct 26 2026)` | firmware + build date |
| `board` | `DX-LR30` | |
| `get role` | `repeater` | |
| `get public.key` | 64 hex chars | Ed25519 pubkey |
| `clock` | epoch seconds | advert timestamp base + uptime |
| `time <epoch>` | `OK` | re-syncs advert clock (companions reject `\|now-ts\| > 3600 s`) |
| `get name` / `set name <n>` | name / `OK` | 23 B max with location, 31 B without |
| `get lat` / `set lat <deg>` | `52.520007` / `OK` | ±90°, 6 decimals; `0` = no location in advert |
| `get lon` / `set lon <deg>` | `13.404954` / `OK` | ±180° |
| `get radio` | `869.618,62.5,8,8` | freq MHz, BW kHz, SF, CR |
| `set radio <f>,<bw>,<sf>,<cr>` | `OK, reboot to apply` | 300–2500 MHz, 7–500 kHz, SF 5–12, CR 5–8 |
| `get freq` / `set freq <MHz>` | `869.618` / `OK, reboot to apply` | EU868 band 863–870 |
| `get tx` / `set tx <dbm>` | `17` / `OK, reboot to apply` | 1–22 dBm |
| `get flood.advert.interval` | `17` | hours, 3–168 (default 47) |
| `set flood.advert.interval <h>` | `OK` | |
| `get advert.interval` | `90` | zero-hop minutes, 0 (off) or 60–240 |
| `set advert.interval <m>` | `OK` | values < 60 clamp to 0 (disabled) |
| `advert` | `OK` | send a flood ADVERT now |
| `advert.zerohop` | `OK` | zero-hop ADVERT + 5×1 s discovery burst |
| `neighbors` | `AABB:<ts>:<snr*4>` lines | 8 most recent adverts heard |
| `discover.neighbors` | `OK` | discovery burst |
| `stats-core` | uptime/name/duty/budget/loop | `budget=` = remaining duty-cycle budget (ms) |
| `stats-packets` | rx/relay/drop/advert counters | |
| `log` | last 20 diagnostic lines | console is silent by default; this is how you read RX/relay/drop/ADVERT/ALIVE lines |
| `log clear` | `OK` | empty the diagnostic buffer |
| `get repeat` / `set repeat on\|off` | `on` / `OK` | relay master switch |
| `get loop.detect` / `set <lvl>` | `minimal` / `OK` | off\|minimal\|moderate\|strict |
| `get txdelay` / `set <0-2>` | `0.5` / `OK` | flood retransmit delay factor |
| `get direct.txdelay` / `set <0-2>` | `0.3` / `OK` | direct traffic delay factor |
| `get dutycycle` / `set <1-100>` | `10` / `OK` | EU default 10 % |
| `get flood.max` / `set <0-64>` | `64` / `OK` | max flood hops |
| `reboot` | *(no reply)* | per MeshCore spec |
| `erase` | `OK` + reboot | **factory reset** — wipes stored settings |
| `dfu` *(if compiled in)* | `OK - entering bootloader in 1s` | see §3.5 |

Example session:

```
> ver
1.4.0 (Oct 26 2026)
> get name
DE.BW.LB-Bi-Bi.STM32
> set name DE.BW.Specter-Roof
OK
> get radio
869.618,62.5,8,8
> set advert.interval 120
OK
> neighbors
F7A9:1793148995:45
> log
RX RSSI=-87 SNR=7 len=110
Relay hop=2
ALIVE uptime=40s ts=1793052700 rx=2 relay=1 drop=1
> reboot
```

### 3.4 Settings storage

All `set` commands persist immediately to the last flash page
(`0x0801F000`, magic + CRC16, see `src/prefs.cpp`). Build flags
(`NODE_NAME_STR`, `NODE_LAT_I`, `NODE_LON_I`) are defaults only — stored
settings win. `erase` returns the node to build-flag defaults.

### 3.5 Flashing without buttons (optional)

The STM32F103 has no USB DFU. The ROM serial bootloader (57600 8E1 on the
same port) can be entered from the CLI **if** the board's BOOT0 and RESET
lines are reachable as GPIOs:

```ini
; platformio.ini build_flags
    -D DFU_BOOT0_PIN=PBxx
    -D DFU_RST_PIN=PBxx
```

then:

```bash
# in the CLI:  dfu            → board drops into ROM bootloader
stm32flash -w .pio/build/specter/firmware.bin -v -R -b 57600 /dev/cu.usbserial-10
```

Without the pins defined, use the physical BOOT0+RESET button sequence
(see [flashing.md](flashing.md)).

## 4. Implementation plan

1. **Preferences storage** — reserve one 1 KB STM32 flash page
   (e.g. end of the 128 KB app region) as a key/value store; wear-lean
   "write-only log" scheme or simple A/B pages. Holds: name, lat, lon,
   radio params, tx, advert intervals, repeat, loop.detect, txdelay,
   dutycycle, flood.max. No EEPROM/Preferences lib exists for this board —
   custom minimal writer (~150 LOC).
2. **CLI parser** — line-based reader on Serial1 (USART1), 64-char line
   buffer, `cmd arg1 arg2` tokenization, dispatched in `loop()` between
   radio ops (radio ops stay interrupt-driven; parse is non-blocking).
   Prompt/echo behavior kept minimal to not interfere with `monitor-log.py`.
3. **Command handlers** — per table above; `set` handlers validate ranges,
   persist, and report `Reboot required` where noted.
4. **Neighbor cache** — 8-entry ring of {pubkey, ts, snr} filled from
   received ADVERT payloads; backs `neighbors` and `discover.neighbors`.
5. **Timestamp re-sync** — `time <epoch>` adjusts `ts_base` so subsequent
   adverts carry correct Unix time.

## 5. Progress log

- **2026-07-15** — Project inspected. Current firmware (v1.1.0) is a
  flood repeater with *no CLI at all*: fixed radio config, hardcoded
  advert intervals (2 min zero-hop / 12 h flood), auto/fixed identity,
  interrupt-driven SX1262 RX, flash-only storage (no preferences yet).
  `CLI.pdf` extracted and analyzed (29 pages, full MeshCore command set).
  Command subset for the Router/repeater role defined above (Tiers 1–3,
  27 commands), EU868 defaults chosen.
- **2026-07-15 (3)** — CLI test-run on hardware (v1.2.1). First attempt
  blocked by **PommeCore holding /dev/cu.usbserial-10** (companion app
  consumes all serial data — close it before CLI work). Full smoke test
  (script: /tmp/cli_test.py, EIO-tolerant CH340 reads) passes:
  all reads/writes, validation, reboot, and flash persistence verified.
  Bugs found & fixed by the test:
  - newlib-nano **stubs out float printf/scanf** — every `%f` printed
    empty (`get lat`/`get radio` → `,,8,8`). All formatting/parsing now
    integer fixed-point (`print_fixed`, `parse_micro`); firmware shrank
    75→61 KB (46.8 % flash) as a side effect.
  - `set name`/`set radio` scanned the line buffer *after* the tokenizer
    null-terminated it in place → "Err: parse". Fixed with a pristine
    `line_raw` copy.
  Flashing workflow proven: buttons into bootloader once, then
  `stm32flash -w .pio/build/specter/firmware.bin -v -R -b 57600 /dev/cu.usbserial-10`
  (bootloader probe: bare `stm32flash -b 57600 <port>`; `-v` is invalid
  without `-w`).
  Added `dfu` CLI command (programmatic bootloader entry: BOOT0 high +
  NRST pulse), compiled in via `-D DFU_BOOT0_PIN=.. -D DFU_RST_PIN=..` —
  awaiting confirmation of which GPIOs the DX-LR30 routes BOOT0/RESET to.
  Test left lat/lon at Berlin (52.520007, 13.404954); no prior stored
  values existed to restore.
- **2026-07-15 (2)** — CLI implemented and building (v1.2.0, `specter`
  env: flash 57.5%, RAM 23.7%). New files: `src/config.h`,
  `src/prefs.cpp`, `src/cli.cpp`; `src/main.cpp` rewritten to runtime
  config.
  Implemented commands: `ver`, `board`, `get role`, `reboot`, `erase`,
  `clock`, `time <epoch>`, `advert`, `advert.zerohop`, `neighbors`,
  `discover.neighbors`, `stats-core`, `stats-packets`, and
  get/set for: `name`, `lat`, `lon`, `radio`, `freq`, `tx`,
  `flood.advert.interval`, `advert.interval`, `repeat`, `loop.detect`,
  `txdelay`, `direct.txdelay`, `dutycycle`, `flood.max`.
  Also: flash prefs page (0x0801F000, magic+CRC16), 8-slot neighbor
  cache, token-bucket duty cycle (see development-notes), `set repeat off` gate,
  `flood.max` hop gate, `loop.detect` gate, timestamp re-sync via
  `time`, discovery burst (5× 1 s zero-hop adverts).
  Notes: `set tx`/`set radio`/`set freq` persist + reboot to apply
  (RadioLib 7.x SX1262 has no runtime `setTxPower`/reconfig API in this
  build). Zero-hop advert is sent as a plain flood advert (protocol has
  no documented zero-hop marker) + discovery burst.
  **Next:** flash to hardware, test CLI over USB (`monitor-log.py` /
  miniterm @115200), verify `set radio` interop with stock MeshCore,
  check flash-write stalls don't trip IWDG in practice.

### Open questions
- ~~CR mapping~~ — resolved: user confirmed RadioLib CR 8 works on this
  HW; CLI `cr` values (5-8) are stored and passed straight to
  `radio.begin()`, default 8 (tested/interop OK).
- Advert intervals: MeshCore spec says saving *any* setting resets a
  zero-hop interval below 60 min to 0. Implemented as: values < 60 are
  clamped to 0 (disabled) at `set advert.interval` time. Matches the
  spec's practical effect.
- `set radio`/`set tx` require radio re-init: apply-on-reboot (persist +
  hint) vs. live re-init. Chose persist + reboot hint (this RadioLib
  build lacks a safe runtime reconfig path while the radio is in RX).
- Zero-hop advert: no protocol-documented marker found; implemented as
  plain advert + 5× 1 s discovery burst. Verify against stock
  MeshCore behavior if `discover.neighbors` interop matters.
- `specter_repeater` env fails to build (pre-existing RadioLib 7.6.0
  ArduinoHal vs. arduino-ststm32 enum mismatch, unrelated to this work).
  `flash.sh` references it — point it at `specter` or fix the env.
