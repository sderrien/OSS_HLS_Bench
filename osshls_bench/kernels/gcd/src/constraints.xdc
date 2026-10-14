## GCD OpenXC7 constraints for Arty A7 (xc7a35tcsg324-1)
##
## Role:
## - Provide the minimum physical constraints needed for the OpenXC7 flow to run.
## - Keep this file usable even when all 32-bit buses are not yet mapped to board connectors.
##
## Goal:
## - Let `make openxc7 XDC=...` proceed without the missing-XDC error.
## - Constrain the essential control pins and timing clock on Arty A7.
## - Leave wide data buses legal but unconstrained to specific package pins so
##   nextpnr can auto-assign them on the selected package.
##
## Important:
## - This is valid for implementation flow bring-up, but still a minimal I/O map.
## - For real hardware usage, map every externally used port bit to actual board pins.

## ----------------------------- Clock -----------------------------------------
## Arty A7 100 MHz oscillator (E3 pin on Arty A7 100T)
create_clock -name sys_clk -period 10.000 [get_ports {clk}]
set_property -dict {IOSTANDARD LVCMOS33 PACKAGE_PIN E3} [get_ports {clk}]

## --------------------------- Control I/O -------------------------------------
## Arty A7 pushbuttons and LED0.
## BTN0 -> rst, BTN1 -> start, BTN2 -> ce, LED0 -> done.
set_property -dict {PACKAGE_PIN D9 IOSTANDARD LVCMOS33 PULLUP TRUE} [get_ports rst]
set_property -dict {PACKAGE_PIN C9 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports start]
set_property -dict {PACKAGE_PIN B9 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports ce]
set_property -dict {PACKAGE_PIN H5 IOSTANDARD LVCMOS33 DRIVE 8 SLEW SLOW} [get_ports done]

## ----------------------------- Demo Data I/O ---------------------------------
## Arty A7 switches:
##   SW0..SW3 drive a[3:0]
##   SW4..SW7 drive b[3:0]
set_property -dict {PACKAGE_PIN A8  IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports {a[0]}]
set_property -dict {PACKAGE_PIN C11 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports {a[1]}]
set_property -dict {PACKAGE_PIN C10 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports {a[2]}]
set_property -dict {PACKAGE_PIN A10 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports {a[3]}]

set_property -dict {PACKAGE_PIN A11 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports {b[0]}]
set_property -dict {PACKAGE_PIN B11 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports {b[1]}]
set_property -dict {PACKAGE_PIN D12 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports {b[2]}]
set_property -dict {PACKAGE_PIN D13 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports {b[3]}]
