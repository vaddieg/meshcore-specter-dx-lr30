# Flashing Guide, SPECTER on DX-LR30

## Overview

The DX-LR30 uses an **STM32F103C8T6** with a factory UART bootloader accessible
via BOOT0 pin. The board has a host-driven **DTR/RTS auto-DFU circuit**
(same idea as the Arduino auto-reset), so bootloader entry is possible
fully in software — see [DTR/RTS auto-DFU](#dtrrts-auto-dfu-headless).
Until that is confirmed on your unit, the physical buttons always work.

```mermaid
flowchart TD
    classDef tool fill:#1e3a5f,stroke:#4a90d9,color:#e8f4fd
    classDef hw fill:#163316,stroke:#5cb85c,color:#e8f5e9
    classDef warn fill:#3a1a1a,stroke:#d9534f,color:#fde8e8

    START([Connect DX-LR30 via USB]):::hw
    CHECK{"/dev/ttyUSB0\nvisible?"}:::tool
    PERM["sudo usermod -aG dialout $USER\nLog out + back in"]:::warn
    BL["Enter bootloader\nHold BOOT0 → Pulse RESET → Release BOOT0"]:::hw
    FLASH["Run flash.sh or stm32flash"]:::tool
    VERIFY{Verify OK?}:::tool
    DONE(["✅ SPECTER running"]):::hw
    ERR(["❌ Check cable / port\nRepeat button sequence"]):::warn

    START --> CHECK
    CHECK -- No --> PERM --> CHECK
    CHECK -- Yes --> BL --> FLASH --> VERIFY
    VERIFY -- Yes --> DONE
    VERIFY -- No --> ERR --> BL
```

---

## Prerequisites

### Software

```bash
# PlatformIO (for building)
pip install platformio

# stm32flash (for flashing)
sudo apt install stm32flash       # Debian/Ubuntu
# or: brew install stm32flash     # macOS
# or: /usr/bin/stm32flash        # may already be present
```

### Permissions

```bash
# Check if your user is in dialout
groups | grep dialout

# If not:
sudo usermod -aG dialout $USER
# Log out and back in, then reconnect the device
```

### Verify device is visible

```bash
ls -la /dev/ttyUSB*
# Expected: /dev/ttyUSB0  (or ttyUSB1, etc.)
```

---

## Method 1: flash.sh (Recommended)

```bash
cd dx-lr30-fw
./flash.sh
```

The script will:
1. Build the firmware with PlatformIO
2. Print step-by-step bootloader entry instructions
3. Wait 10 seconds for you to press the buttons
4. Run stm32flash automatically

```bash
./flash.sh --no-build   # skip build, flash existing .bin
./flash.sh --monitor    # flash + open serial monitor at 115200
```

---

## Method 2: Manual Steps

### Step 1, Build

```bash
cd dx-lr30-fw
pio run -e specter
# Output: .pio/build/specter/firmware.bin
```

### Step 2, Enter Bootloader

```
┌─────────────────────────────────────────────────────┐
│  DX-LR30 top view (USB-C connector on left)         │
│                                                     │
│  [USB-C]  ┌──────────────────────────────────────┐ │
│           │                                      │ │
│           │   [BOOT0 btn]          [RESET btn]   │ │
│           │       ●                    ●         │ │
│           └──────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘

Sequence:
  1. Hold BOOT0 (keep pressed throughout)
  2. Press and release RESET
  3. Wait ~100ms
  4. Release BOOT0

The LED stays ON, the bootloader is running.
```

```mermaid
sequenceDiagram
    participant You
    participant HW as DX-LR30

    You->>HW: 1. Press and HOLD BOOT0
    You->>HW: 2. Press RESET
    You->>HW: 3. Release RESET
    Note over HW: STM32 samples BOOT0=HIGH<br/>→ starts UART bootloader
    You->>HW: 4. Release BOOT0
    Note over HW: LED stays ON<br/>Ready for stm32flash @ 57600 8E1
```

> ⚠️ **CRITICAL:** This board requires `-b 57600` (not 115200). See [development-notes.md](development-notes.md#critical-stm32-bootloader-baud-rate) for details.

### Step 3, Flash

```bash
stm32flash \
  -w .pio/build/specter/firmware.bin \
  -v \
  -R \
  -b 57600 \
  /dev/ttyUSB0
```

**Expected output:**

```
stm32flash 0.7

http://stm32flash.sourceforge.net/

Using Parser : Raw BINARY
Interface serial_posix: 57600 8E1
Version      : 0x22
Option 1     : 0x00
Option 2     : 0x00
Device ID    : 0x0410 (STM32F10xxx Medium-density)
- RAM        : Up to 20KiB  (512b reserved by bootloader)
- Flash      : Up to 128KiB (size first sector: 4x1024)
- Option RAM : 16b
- System RAM : 2KiB
Write to memory
Erasing memory
Wrote and verified address 0x0800a5c0 (100.00%) Done.

Starting execution at address 0x08000000... done.
```

### Step 4, Confirm

Open serial monitor immediately after flash:

```bash
python3 -m serial.tools.miniterm /dev/ttyUSB0 115200
```

Expected:
```
=== SPECTER MeshCore Repeater [SPECTER-A3F2] ===
Hash: 0xF4
Radio init... OK
Sending initial ADVERT...
Listening...
```

---

## Troubleshooting

### `Failed to init device`

```
stm32flash: Failed to init device.
```

**Cause:** The STM32 is not in bootloader mode, or wrong baud/port.

**Fix:**
1. **Use 57600 baud** (this board does NOT work reliably at 115200)
2. Repeat the button sequence, it takes practice
3. The LED should stay solid ON while in bootloader (not blinking)
4. Verify the port: `ls /dev/ttyUSB*` and adjust the command

---

### `Permission denied: /dev/ttyUSB0`

```bash
sudo chmod a+rw /dev/ttyUSB0   # temporary fix
# or:
sudo usermod -aG dialout $USER  # permanent fix (requires logout)
```

---

### No output on serial monitor

**Most common cause:** USART mapping issue (if you built custom firmware).

The Arduino STM32 framework maps `Serial` to USART2 by default.
The DX-LR30 CH340 is connected to USART1. The `ENABLE_HWSERIAL1` build
flag **must** be present in `platformio.ini`:

```ini
build_flags =
    -D HAL_UART_MODULE_ENABLED
    -D ENABLE_HWSERIAL1       ← critical
    -D RADIOLIB_STATIC_ONLY=1
    -O2
```

And in `src/main.cpp`:

```cpp
Serial1.begin(115200);    // ← must be Serial1, not Serial
```

---

### `Radio init FAILED`

Check SPI wiring (software bug, not normally an issue with stock firmware).
In production, this causes the LED to blink continuously and the device
to halt. Verify all 8 SPI/control pins are intact.

---

## DTR/RTS auto-DFU (headless)

The schematic shows a classic host-driven auto-DFU circuit on the CH340:

| Transistor       | Driven by         | Controls                        | Effect                        |
|------------------|-------------------|---------------------------------|-------------------------------|
| Q1 (S8050, NPN)  | DTR# → R16 (1K)   | NRST (R15 10K pull-up + D1)     | DTR# HIGH → NRST LOW (reset)  |
| Q2 (S8550, PNP)  | RTS# → R17 (1K)   | BOOT0 (R19 10K pull-down)       | RTS# LOW → BOOT0 HIGH (bootloader) |

The CH340's DTR#/RTS# outputs are active-low with respect to the host
termios bits, which translates to:

- **host `rts = 1`** → RTS# LOW → Q2 conducts → **BOOT0 armed (HIGH)**
- **host `dtr: 1 → 0 → 1`** → DTR# HIGH while deasserted → Q1 conducts →
  **NRST pulled LOW, then released = reset pulse**

Entry: `rts=1`, then a DTR falling pulse (hold ≥ 100 ms). Exit: `rts=0`,
same DTR pulse → boots the application.

### Why the old "DTR/RTS doesn't work" note was wrong

The earlier failed tests used sequences that could not work with this circuit:

| Old test | What it actually did |
|----------|----------------------|
| `stm32flash -i rts,-dtr` | Armed BOOT0, then drove DTR low and **never released it → MCU held in reset forever** (a timeout is the *expected* result) |
| `stm32flash -i dtr,-rts` | No reset edge at all |

The schematic-derived sequence has not been properly tested yet.

### Flashing (once the sequence is confirmed)

```bash
PORT=/dev/ttyUSB0   # Linux, real ch341 driver required
FW=.pio/build/specter/firmware.bin

stm32flash -i ',rts,-dtr,,,dtr,,,,:-rts,dtr,-dtr,,,dtr,,,,' \
    -w "$FW" -v -b 57600 "$PORT"
```

- Entry sequence: settle, `rts=1` (arm BOOT0), DTR falling pulse (reset),
  settle → ROM bootloader at 57600 8E1.
- Exit sequence: `rts=0` (disarm), DTR pulse → MCU boots the new app.
- Or interactively: `.flashenv/bin/python tools/probe_dtr_rts.py $PORT`
  sweeps the polarities and reports which one gets the bootloader ACK.

> **Host driver caveat:** the sequence must be tested on Linux with the real
> `ch341` kernel driver. On macOS the CH340 is served by Apple's generic
> CDC-ACM `usb.serial` DriverKit driver, which is known not to reliably
> propagate DTR/RTS to the CH340's pins — DTR/RTS tests there are
> inconclusive (verified: no reset edge reaches the MCU from macOS).

> **If the Linux test still times out**, the unit may differ from the
> schematic: check the CH340 part marking (CH340G has DTR#/RTS# pins;
> CH340C/clone variants often do not bring them out) and whether Q1/Q2 are
> populated. A multimeter on the NRST net while pulsing DTR settles it.

---

## Physical buttons (always works)

Hold BOOT0, press RESET, release RESET, release BOOT0, then flash at
57600 8E1.

---

## Re-flashing

You can re-flash at any time:
- The firmware does not lock flash or set read protection
- Just repeat the BOOT0+RESET button sequence before running `stm32flash`
- The `-R` flag in `stm32flash` resets the device automatically after flash

---

## Factory Reset

There is no factory firmware to restore, the original AT firmware was
replaced. If you want to go back to AT-command mode, you would need to
obtain the original DX-LR30 firmware binary from the manufacturer and
flash it using the same procedure.
