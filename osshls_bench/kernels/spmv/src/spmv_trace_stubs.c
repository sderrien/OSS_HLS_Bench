#include <stdint.h>

/*
 * Native benchmark trace stubs.
 *
 * Role:
 * Provide no-op definitions for source-level `trace(...)` helper calls when the
 * software golden executable is linked.
 *
 * Goal:
 * Keep explicit L3/RTL bisimulation observation points in `spmv.c` without
 * changing native functional behavior.
 */
void __ohls_vcd_trace_i8(const char *name, uint8_t value) {
    (void)name;
    (void)value;
}

void __ohls_vcd_trace_i16(const char *name, uint16_t value) {
    (void)name;
    (void)value;
}

void __ohls_vcd_trace_i32(const char *name, uint32_t value) {
    (void)name;
    (void)value;
}

void __ohls_vcd_trace_i64(const char *name, uint64_t value) {
    (void)name;
    (void)value;
}
