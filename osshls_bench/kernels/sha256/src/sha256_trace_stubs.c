#include <stdint.h>

/*
 * Source-level trace helper stubs for native SHA-256 testbench builds.
 *
 * Role:
 * Provide no-op definitions for the typed helper calls emitted by
 * `trace(value)` in `sha256.c` when the software golden executable is linked.
 *
 * Goal:
 * Keep the kernel source explicit about `ohls.trace.primary` observation points
 * while preserving software semantics: native C execution ignores trace markers,
 * and OHLS frontend lowering still sees the external helper calls in the kernel
 * translation unit before they are converted to trace metadata.
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
