#!/usr/bin/env python3
"""
probe_dtr_rts.py — empirically find the DTR/RTS sequence that boots the
DX-LR30 (STM32F103) into the ROM UART bootloader.

Circuit (from schematic):
  Q1 S8050 NPN: DTR# -> R16(1K) -> base, collector -> NRST (R15 10K pull-up)
  Q2 S8550 PNP: RTS# -> R17(1K) -> base, emitter  -> BOOT0 (R19 10K pull-down)
CH340 DTR#/RTS# pins are active-low w.r.t. the host termios bits.

For each candidate (boot0_level, reset pulse polarity) we:
  1. set rts = boot0_level
  2. pulse dtr (reset)
  3. wait, then send bootloader INIT (0x7F) at 57600 8E1 and look for ACK (0x79)

If nothing works, the board is left with rts=False (BOOT0 low = normal boot)
and one final reset pulse is NOT issued unless a working combo was found.

IMPORTANT: run this on Linux with the real ch341 kernel driver. On macOS the
CH340 is served by Apple's generic CDC-ACM usb.serial driver which does not
reliably propagate DTR/RTS to the chip pins — results there are inconclusive.

Usage: python3 tools/probe_dtr_rts.py [port]   # e.g. /dev/ttyUSB0
"""

import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
BAUD = 57600
ACK = 0x79
INIT = 0x7F


def open_port():
    # 57600 8E1 — ROM bootloader parameters. Opening also asserts dtr+rts
    # (pyserial defaults), which on this board already arms BOOT0 (rts=1).
    s = serial.Serial(PORT, BAUD, bytesize=8, parity="E", stopbits=1, timeout=1)
    time.sleep(0.2)
    return s


def try_init(s, tries=5, settle=0.4):
    """Send INIT and look for the bootloader ACK (57600 8E1)."""
    s.baudrate = BAUD
    s.parity = serial.PARITY_EVEN
    s.timeout = settle
    time.sleep(0.15)
    for _ in range(tries):
        s.reset_input_buffer()
        s.write(bytes([INIT]))
        time.sleep(settle)
        data = s.read(16)
        if bytes([ACK]) in data:
            return True
    return False


def app_booted(s, t=2.0):
    """Look for the SPECTER app boot banner (115200 8N1)."""
    s.baudrate = 115200
    s.parity = serial.PARITY_NONE
    s.timeout = 0.5
    time.sleep(0.15)
    buf = b""
    end = time.time() + t
    while time.time() < end:
        buf += s.read(1024)
        if b"===" in buf:
            return True
    return False


def pulse(s, hi_first: bool, width=0.15):
    s.dtr = hi_first
    time.sleep(0.05)
    s.dtr = not hi_first
    time.sleep(width)
    s.dtr = hi_first
    time.sleep(0.05)


def main():
    s = open_port()

    # 0) Maybe the board is already in the bootloader?
    if try_init(s, tries=2):
        print("BOARD ALREADY IN BOOTLOADER (no sequence needed)")
        s.rts = False  # disarm BOOT0 for next reset
        s.close()
        return

    # Candidates: (label, rts_level, dtr pulse hi_first)
    # Expected winner from the schematic: rts=1 (BOOT0 hi), pulse 1->0->1.
    candidates = [
        ("rts=1, dtr 1->0->1", True,  True),
        ("rts=0, dtr 1->0->1", False, True),
        ("rts=1, dtr 0->1->0", True,  False),
        ("rts=0, dtr 0->1->0", False, False),
    ]

    for label, rts, hi_first in candidates:
        print(f"try: {label} ...", flush=True)
        s.rts = rts
        time.sleep(0.1)
        pulse(s, hi_first)
        time.sleep(0.5)  # let the MCU boot
        if try_init(s):
            print(f"SUCCESS: {label}")
            print("Board is now in the ROM bootloader (57600 8E1).")
            print("Next:  stm32flash -w <fw.bin> -v -b 57600", PORT)
            print("To boot app afterwards:  rts=0, then dtr 1->0->1 pulse")
            s.close()
            return
        if app_booted(s):
            print("  reset OK (app banner) but BOOT0 not armed with this combo")
        else:
            print("  no reset at all (no banner, no ACK) — DTR/RTS not reaching MCU?")

    # Nothing worked — leave the board in a safe state (normal boot on next reset)
    s.rts = False
    s.dtr = True
    s.close()
    print("No candidate produced a bootloader ACK.")
    print("Possibilities: DTR/RTS not wired to this CH340, app firmware is")
    print("holding the UART, or different timing. Board left with RTS low.")
    sys.exit(1)


if __name__ == "__main__":
    main()
