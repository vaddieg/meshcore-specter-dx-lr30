#!/usr/bin/env python3
"""Regenerate the battery-mod schematic (docs/battery-mod.svg / .png).

This is the diagram referenced from hardware.md, "Power -> Battery mod".
It draws the solar.md "Recommended way" wiring.

Regenerate (needs the schemdraw + matplotlib stack, NOT a project dependency):

    python3 -m venv /tmp/schemenv
    /tmp/schemenv/bin/pip install "schemdraw[matplotlib]"
    /tmp/schemenv/bin/python docs/gen_battery_mod.py

Writes battery-mod.{svg,png} next to this script.
"""
import os
import matplotlib
matplotlib.use("Agg")
import schemdraw
import schemdraw.elements as elm

HERE = os.path.dirname(os.path.abspath(__file__))

d = schemdraw.Drawing()
d.config(fontsize=13)
GAP = 1.5

# ── TP4056 charger module ───────────────────────────────────────────────────
tp = (elm.Ic(size=(4, 5)).label('TP4056\nLi-Ion charger')
      .side('L', spacing=2, pad=2)
      .side('R', spacing=2, pad=2)
      .side('B', spacing=2, pad=2)
      .pin(name='IN+',  side='left',  pin='1')
      .pin(name='IN-',  side='left',  pin='2')
      .pin(name='OUT+', side='right', pin='1')
      .pin(name='OUT-', side='right', pin='2')
      .pin(name='BAT+', side='bot',   pin='1')
      .pin(name='BAT-', side='bot',   pin='2'))
d += tp
INp, INn   = tp['IN+'], tp['IN-']
OUTp, OUTn = tp['OUT+'], tp['OUT-']
BATp, BATn = tp['BAT+'], tp['BAT-']

# ── Solar panel (left, gapped) ──────────────────────────────────────────────
solar = (elm.Ic(size=(3, 3.5)).label('SOLAR PANEL\n5 V / 2 W')
         .side('R', spacing=2, pad=2)
         .pin(name='+', side='right', pin='1')
         .pin(name='-', side='right', pin='2')
         .anchor('+').at((INp.x - GAP, INp.y)))
d += solar
d += elm.Line().endpoints(solar['+'], INp).label('charge', 'top')
d += elm.Line().endpoints(solar['-'], INn)

# ── 1N5817 diode (on OUT+ line, gapped) + DX-LR30 board (right) ─────────────
diode = elm.Diode(fill=True).at((OUTp.x + GAP, OUTp.y)).label('1N5817', 'top')
d += diode
board = (elm.Ic(size=(3, 4)).label('DX-LR30')
         .side('L', spacing=2, pad=2)
         .pin(name='3V3', side='left', pin='1')
         .pin(name='GND', side='left', pin='2')
         .anchor('3V3').at((diode.end.x + GAP, diode.end.y)))
d += board
d += elm.Line().endpoints(OUTp, diode.start)
d += elm.Line().endpoints(diode.end, board['3V3']).label('power', 'top')
d += elm.Line().endpoints(OUTn, board['GND'])

# ── 18650 battery (below, centered under the BAT pair) ──────────────────────
bcx = (BATp.x + BATn.x) / 2
bat = elm.Battery().down().at((bcx, BATp.y - GAP)).label('18650\nLi-Ion', 'right')
d += bat
d += elm.Line().endpoints(BATp, (BATp.x, bat.start.y))
d += elm.Line().endpoints((BATp.x, bat.start.y), bat.start)
d += elm.Line().endpoints(BATn, (BATn.x, bat.end.y))
d += elm.Line().endpoints((BATn.x, bat.end.y), bat.end)

d.save(os.path.join(HERE, "battery-mod.png"))
d.save(os.path.join(HERE, "battery-mod.svg"))
print("wrote", os.path.join(HERE, "battery-mod.{png,svg}"))
