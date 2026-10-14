#include <stdint.h>

/*
 * Native benchmark trace stubs.
 *
 * Role: provide no-op implementations for the source-level `trace(...)`
 * helper when the software reference executable is linked.
 *
 * Goal: keep the benchmark kernel instrumented for VCD/debug flows without
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
