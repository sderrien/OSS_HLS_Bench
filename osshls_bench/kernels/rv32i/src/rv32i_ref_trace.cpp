/*
 * Role:
 * Provide a second copy of the RV32I reference interpreter under a distinct
 * symbol name (`cpu_run_ref`) for trace generation in compiled cycle-sim runs.
 *
 * Goal:
 * Allow the compiled cycle-sim binary to keep using `cpu_run` for lowered
 * hardware execution while still being able to emit the canonical text trace
 * used by file-diff verification.
 */
#define cpu_run cpu_run_ref
#include "rv32i_kernel.cpp"
