# RV32I QoR Table

Target FPGA: `xc7a100tcsg324-1`.
Timing constraint: `10.0 ns`.
Area and timing are Vivado post-synthesis results.

| Tool/config | Cycles | II | Tclk (ns) | LUT | FF | LUTRAM | BRAM | DSP |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| OpenHLS, RAM32X1S | 744 | 5 | 8.026 | 1,647 | 1,007 | 32 | 0 | 0 |
| Vitis, pipelined | unavailable | 5 | 7.927 | 1,557 | 525 | 0 | 1 | 0 |
| Vitis, non-pipelined | 913 | - | 8.004 | 1,540 | 440 | 0 | 1 | 0 |

## Measurement Notes

- OpenHLS cycles are the exact RTL simulation count from the common RV32I GCD testbench.
- The OpenHLS register file uses 32 `RAM32X1S` distributed-RAM primitives.
- Vitis pipelined RTL does not assert `ap_done` during co-simulation, so an exact cycle count is unavailable.
- Vitis non-pipelined cycles are the exact co-simulation latency using the same GCD testbench.
