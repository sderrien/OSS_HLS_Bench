## GCD Example

This tutorial folder contains a cycle-accurate Verilog implementation of the
greatest common divisor (GCD) kernel. A simple FSM loads the two inputs when
`start` is asserted, subtracts the smaller operand from the larger one each
cycle, and raises `done` once both operands equal the GCD.

The RTL pipeline in `simulation/rtl/example` now consumes this Verilog (`kernel.v`)
directly, so `make run` lowers a prebuilt GCD module, generates a Verilator
testbench, and emits `kernel.vcd` while the C harness drives `start`, collects
`done`, and prints the final value.

## Arty A7 Bring-Up Notes

For FPGA bring-up with OpenXC7, use `documentation/tutorial/gcd/constraints.xdc`.
That constraints file maps the control and demo I/O as follows:

- `clk` -> 100 MHz oscillator (pin E3 on Arty A7 100T)
- `rst` -> BTN0
- `start` -> BTN1
- `ce` -> BTN2
- `done` -> LED0
- `a[3:0]` -> SW0..SW3
- `b[3:0]` -> SW4..SW7
- `ret0[6:0]` -> LED1..LED7

Practical usage flow on board:

1. Set `SW0..SW3` to operand `a` (0-15).
2. Set `SW4..SW7` to operand `b` (0-15).
3. Hold `ce=1` (press BTN2 in this demo mapping).
4. Pulse `rst` (BTN0) once to clear state.
5. Pulse `start` (BTN1) once to load inputs and begin.
6. Wait for `done` (LED0) and read `ret0` on LED1..LED7.

Example: `a=12 (1100)` and `b=8 (1000)` should produce `ret0=4`.
