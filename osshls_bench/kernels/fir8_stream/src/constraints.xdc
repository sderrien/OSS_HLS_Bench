## FIR8 Stream OpenXC7 constraints for Arty A7 (xc7a100tcsg324-1)
##
## Role:
## - Provide the essential clock/control pin mapping so the OpenXC7 flow
##   converges on the Arty A7 board.
## - Leave wide streaming data paths unconstrained so nextpnr can automatically
##   assign them as the flow matures.
##
## Goal:
## - Apply the same minimal pin mapping that works for the gcd tutorial while
##   clarifying these ports are used by the streaming demo (in/out_valid,
##   ready, data, etc.).
## - Keep future iterations drop-in compatible with other example data paths.

## ----------------------------- Clock -----------------------------------------
create_clock -name sys_clk -period 10.000 [get_ports {clk}]
set_property -dict {IOSTANDARD LVCMOS33 PACKAGE_PIN E3} [get_ports {clk}]

## --------------------------- Control I/O -------------------------------------
set_property -dict {PACKAGE_PIN D9 IOSTANDARD LVCMOS33 PULLUP TRUE} [get_ports rst]
set_property -dict {PACKAGE_PIN C9 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports start]
set_property -dict {PACKAGE_PIN B9 IOSTANDARD LVCMOS33 PULLDOWN TRUE} [get_ports ce]
set_property -dict {PACKAGE_PIN H5 IOSTANDARD LVCMOS33 DRIVE 8 SLEW SLOW} [get_ports done]

## ----------------------------- Stream I/O -----------------------------------
## The streaming data/control pins remain unconstrained to let nextpnr place them
## freely; the flow is primarily concerned with bringing up the streaming
## hardware logic rather than guessing PCB pin assignments.
