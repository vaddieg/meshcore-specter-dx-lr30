# RX duty cycle (`radio.rxps`) — investigation & status

**Status: POSTPONED.** The native SX1262 RX duty cycle does not reliably work
on the DX-LR30 board. The feature is implemented on this branch (`rx_duty`) but
is **not merged to `main`** and should not be relied on. This document records
what was tried, what was found, and what would be needed to revisit it.

> Reference for the CLI surface (as implemented) is still in
> [cli.md §3.7](cli.md#37-rx-duty-cycle-power-saving).

## Goal

Cut the idle current of a battery/off-grid repeater by letting the LoRa chip
sleep between short RX windows, matching the community "RX PowerSaving" (RXPS)
feature in the IoTThinks EasySkyMesh / MeshCore forks. SPECTER already does the
*other* power layer (MCU idles in `__WFI()` between interrupts); RXPS is the
layer that puts the **SX1262 itself** to sleep.

## Approach implemented

- `set radio.rxps <0-10>` / `get radio.rxps` (numeric only — SPECTER does **not**
  implement the IoTThinks named profiles `on`/`conservative`/`balanced` or the
  `powersaving` MCU-sleep command).
- Armed via the native driver call `radio.startReceiveDutyCycle(rxp, slp)`:
  - `rxp = radio.getTimeOnAir(MAX_PKT) / 1000` (RX window ≈ a full max frame)
  - `slp = rxp * level` (sleep window; level 1 ≈ 50 % duty … level 10 ≈ 9 % at SF8/62.5 kHz)
- The driver subtracts `transitionTime = tcxoDelay + 1000` from `slp` before the
  24-bit period check; on this board `tcxoDelay = 0`, so 1 s is subtracted.
- **Admin gate:** the duty cycle is suspended while a mesh-admin session is
  active (`acl_count() > 0`) so a remote admin can always reach the node; it
  resumes on the next RX re-arm.
- The level is stored in the node-config flash record (fits existing padding;
  on-flash layout/CRC unchanged, pre-existing devices default to off).

## What the reference firmware actually does (the part we first got wrong)

The IoTThinks approach — now **merged into upstream MeshCore** — sizes the LoRa
**preamble** per spreading factor:

```c
// MeshCore src/helpers/radiolib/RadioLibWrappers.h
static uint16_t preambleLengthForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }
```

RXDC only latches a packet whose **preamble overlaps a wake window** (the chip
sees the preamble, stays awake, and takes the rest of the frame). SPECTER had
armed RXDC with a fixed **8-symbol** preamble (~33 ms at SF8/62.5 kHz) against a
multi-second sleep gap — far too short to bridge it, which is the first reason
the node went fully deaf. The fix was to match the official value (32 for SF≤8,
16 for SF9–12) in `radio.begin()`. Note the preamble is a **single SX1262
register shared by TX and RX**, so this also lengthens our own TX preamble by
~33 ms on SF8 (negligible) and is what keeps us mutually audible with
official-firmware peers (which transmit 32 on SF8).

## Findings (empirical, on the DX-LR30)

1. **8-symbol preamble + RXDC → fully deaf.** Expected; preamble can't bridge the gap.
2. **32-symbol preamble (official value), level 1 → briefly worked** (RX count
   climbed off zero after a reboot), then **stopped working even at level 1**.
3. **Level 5 → deaf** (`rx=0` across a 4-minute window).

The catch rate is expected to fall as the sleep gap grows (wake window is fixed
at ≈ max-frame airtime), roughly `rxWindow / (rxWindow + sleepGap)`:

| level | sleep gap (SF8/62.5) | ~catch fraction |
|---|---|---|
| 1 | ~1.1 s | ~50 % |
| 3 | ~3.3 s | ~25 % |
| 5 | ~4.8 s | ~19 % |
| 10 | ~9.6 s | ~10 % |

So level 5 being lossy is consistent with the model — but **level 1 also
degrading to dead** is *not* explained by the model and is the core open
problem. A working RXDC should at least catch ~half of packets at level 1.

## Power

The board has a **host-controlled RF switch** (`RXEN=PA1`, driven high for the
whole cycle by `rf_rx()`). Per the IoTThinks wiki, on such boards the switch —
and any LNA behind it — **stays biased through the sleep windows**, which caps
the real saving well below the `rx/sleep` ratio. The originally reported
~30 mA → ~60 mA *rise* under RXDC was never cleanly root-caused; it is
consistent with the chip not settling into the standby half of the cycle (e.g.
the "preamble seen → stay awake" path misfiring), but that is a hypothesis, not
a confirmed cause.

## Open questions (why it's postponed, not concluded)

- Why does **level 1** degrade from "working" to "dead"? (Repro: flash,
  `set radio.rxps 1`, reboot — it worked once, then did not.)
- Is the DX-LR30's **SX1262 a clone** with a broken/quirky RXDC state machine?
  (The board is a clone part; RXDC is a less-exercised code path than plain RX.)
- Does the **RXEN/LNA bias** interact badly with the chip's internal sleep
  (e.g. holding the front end up in a way that defeats the standby current)?
- Is there a **driver-level** issue with `startReceiveDutyCycle` on this
  RadioLib version / this part (period clamping, IRQ flag defaults)?

## What it would take to revisit

1. **Isolate the radio:** confirm plain continuous RX (`radio.rxps 0`) is
   rock-solid first, then A/B RXDC level 1 with a **known-good (genuine)
   SX1262** to rule out a clone-part RXDC bug.
2. **Scope DIO1 / the front end** across an RXDC cycle to see whether the chip
   actually enters standby and whether RXEN/LNA is held up as expected.
3. **Try the `PREAMBLE_DETECTED` IRQ path** the reference firmware uses
   (`startReceive` with the preamble bit, and an `isReceiving()` poll that
   clears the IRQ after a timeout) instead of the blind
   `startReceiveDutyCycle(rxp, slp)` — that gives the host explicit control
   over the wake window and is more debuggable.
4. **Re-measure current** at a confirmed-working level before judging whether
   the saving is worth it once the RXEN/LNA bias is accounted for.

## Bottom line

RXDC is a real power lever on boards where it works, but on the DX-LR30 it is
not reliable as implemented and the failure mode (even level 1 going dead) is
not understood. Keep `main` on continuous RX (`radio.rxps 0`, the default)
until the open questions above are answered.
