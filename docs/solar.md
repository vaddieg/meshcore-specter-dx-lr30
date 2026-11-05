# Designing a cheap solar node
Since the DX-LR30 hardware is dirty cheap already, the goal here is to design the cheapest possible
node capable of running outdoor autonomously for months without maintenance.

## Hardware findings
- There are no pins for connecting the battery on the board
- The AMS1117-3.3 voltage regulator drops whopping 1.1V, making +5V pin useless for battery power supply
- Regulator's input is connected directly to +5V line, its output to 3V3 line 💡
- VBAT is disconnected on this board, so no easy measurements
- STM32F1 is tolerant to voltages up to 4.0V
- DX-LR30 mainboard has enough space under the 900M22S module to fit a TP4056 module 💡

## Easy (but wasteful) way
Use ready modules like BMS, DC buck converter, solar controller. Interconnect with provided cables.
There're plenty of instructions for USB-powered boards. Little to no soldering skills needed

# Recommended way
> If you have some soldering skill, you should have all the necessary components already
- few Li-Ion cells from disposable vapes, or a single 18650 cell
- TP4056 charging module
- a Shottky diode, rated for 0.5-1.0A e.g. 1N5817
- a 2W 5V solar panel


## Hardcore mod
> Advanced soldering skills required, otherwise risk of damaging the board is high!
- Desolder AMS1117
- Solder TP4056 IN+ to the pad where AMS1117's input was 
- Solder TP4056 OUT+ to the pad where AMS1117's output was (optionally via Shottky)
- Solder TP4056 OUT- to the pad where AMS1117's GND was

Since TP4056 doesn't actually stabilize the output voltage, the board modded in 'hardcore' way
will only work properly with a battery installed