"""Shared deterministic benchmark inputs for cross-tool cycle comparisons.

Role:
Provide one source of truth for the small benchmark inputs used by LegUp, Bambu,
and Dynamatic simulation flows.

Goal:
Keep exact cycle counts comparable by ensuring every tool sees the same kernel
arguments and memory contents for a given benchmark.  The data here is deliberately
small, deterministic, and matched to the benchmark-local C harnesses where those
harnesses already existed.

How:
The module builds typed scalar and array values in Python, can emit a C main() for
tool-native testbench generation, and can emit byte-addressed Verilog memory
initialization for LegUp's generated RTL wrapper.  Floating-point values are stored
and emitted through their IEEE-754 bit patterns so RTL memory contents match C
single-precision arrays exactly.
"""

from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Iterable


@dataclass(frozen=True)
class InputObject:
    """One kernel argument value used by all simulation flows.

    Role:
    Describe either a scalar argument or a flattened array argument with enough
    type information to generate both C declarations and LegUp memory bytes.

    Goal:
    Avoid each tool runner inventing separate deterministic inputs.
    """

    name: str
    c_type: str
    shape: tuple[int, ...]
    values: tuple[int | float, ...]
    is_output: bool = False
    is_const: bool = False

    @property
    def is_scalar(self) -> bool:
        return not self.shape

    @property
    def element_count(self) -> int:
        count = 1
        for dim in self.shape:
            count *= dim
        return count


@dataclass(frozen=True)
class BenchmarkInputs:
    """Complete argument list for one benchmark kernel simulation."""

    benchmark: str
    top_function: str
    return_type: str
    header: str | None
    args: tuple[InputObject, ...]


def get_benchmark_inputs(benchmark: str) -> BenchmarkInputs | None:
    """Return the shared input specification for a supported benchmark.

    Role:
    Centralize the benchmark name dispatch used by LegUp, Bambu, and Dynamatic.

    Goal:
    Make unsupported benchmarks explicit rather than silently falling back to
    tool-specific input choices.
    """

    builders = {
        "gcd": _gcd_inputs,
        "dotprod": _dotprod_inputs,
        "matmul": _matmul_inputs,
        "spmv": _spmv_inputs,
        "passthrough": _passthrough_inputs,
        "qr_givens": _qr_givens_inputs,
        "lbm": _lbm_inputs,
        "histogram": _histogram_inputs,
        "iir": _iir_inputs,
        "iterative_sqrt": _iterative_sqrt_inputs,
        "rv32i": _rv32i_inputs,
        "rv32i_comet": _rv32i_comet_inputs,
        "rv32i_truecomet": _rv32i_truecomet_inputs,
    }
    builder = builders.get(benchmark)
    return builder() if builder else None


def generate_c_harness(spec: BenchmarkInputs, *, use_dynamatic_call: bool, write_output_file: bool = False) -> str:
    """Generate a C main() that calls the kernel with the shared inputs.

    Role:
    Feed Bambu's ``--generate-tb`` path and Dynamatic's ``CALL_KERNEL`` verifier
    path from the same benchmark input specification.  When ``write_output_file``
    is set, emit a VibeHLS-compatible ``argv[1]`` output file containing scalar
    return values and byte checksums of array arguments.

    Goal:
    Make C-driven RTL simulations exercise identical scalar values and array
    contents across tools while allowing OpenHLS/VibeHLS to compare golden and RTL
    output files without suppressing functional checking.
    """

    lines: list[str] = ["#include <stdint.h>", "#include <stdio.h>"]
    if write_output_file and spec.benchmark == "qr_givens":
        lines.append("#include <math.h>")
    if spec.header:
        lines.append(f'#include "{spec.header}"')
    if use_dynamatic_call:
        lines.append('#include "dynamatic/Integration.h"')
    else:
        # Benchmark headers may only carry constants, so the generated tool
        # harness provides the callable kernel prototype explicitly.
        proto_args = ", ".join(_prototype_arg(arg) for arg in spec.args)
        lines.append(f"{spec.return_type} {spec.top_function}({proto_args});")
    lines.append("")

    for arg in spec.args:
        if arg.is_scalar:
            continue
        lines.extend(_emit_c_array(arg))
        lines.append("")

    if write_output_file and spec.benchmark == "qr_givens":
        lines.extend(_qr_givens_reference_function())
        lines.append("")

    if write_output_file:
        lines.append("int main(int argc, char **argv) {")
        lines.append("  FILE *out = stdout;")
        lines.append("  if (argc > 1) {")
        lines.append("    out = fopen(argv[1], \"w\");")
        lines.append("    if (!out) {")
        lines.append('      fprintf(stderr, "failed to open output file\\n");')
        lines.append("      return 1;")
        lines.append("    }")
        lines.append("  }")
    else:
        lines.append("int main(void) {")
    for arg in spec.args:
        if arg.is_scalar:
            lines.append(f"  {arg.c_type} {arg.name} = {_c_literal(arg.c_type, arg.values[0])};")
        else:
            dims = "".join(f"[{dim}]" for dim in arg.shape)
            lines.append(f"  {arg.c_type} {arg.name}{dims};")
            lines.append(f"  init_{arg.name}({arg.name});")
    call_args = ", ".join(arg.name for arg in spec.args)
    if use_dynamatic_call:
        lines.append(f"  CALL_KERNEL({spec.top_function}, {call_args});")
    elif spec.return_type == "void":
        lines.append(f"  {spec.top_function}({call_args});")
        if write_output_file:
            if spec.benchmark == "qr_givens":
                _emit_qr_givens_tolerance_check(lines)
            else:
                _emit_output_checks(lines, spec)
    else:
        lines.append(f"  {spec.return_type} result = {spec.top_function}({call_args});")
        if write_output_file:
            lines.append('  fprintf(out, "result=%d\\n", (int)result);')
            _emit_output_checks(lines, spec)
        else:
            lines.append('  fprintf(stdout, "result=%d\\n", (int)result);')
    if write_output_file:
        lines.append("  if (out != stdout) fclose(out);")
    lines.append("  return 0;")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def _qr_givens_reference_function() -> list[str]:
    """Emit the software reference used for OpenHLS QR output checking.

    Role:
    Generate the same Givens-rotation reference calculation as the native
    OpenHLS ``qr_givens`` testbench.

    Goal:
    Check floating-point RTL output numerically instead of requiring bit-exact
    byte checksums, since OpenHLS RTL may differ by small implementation-level
    rounding while still satisfying the benchmark tolerance.
    """

    return [
        "static void qr_ref(const float a[8][8], float ref[8][8]) {",
        "  for (int i = 0; i < 8; ++i) {",
        "    for (int j = 0; j < 8; ++j) {",
        "      ref[i][j] = a[i][j];",
        "    }",
        "  }",
        "  for (int j = 0; j < 8; ++j) {",
        "    for (int i = 7; i > j; --i) {",
        "      float a1 = ref[i - 1][j];",
        "      float a2 = ref[i][j];",
        "      float rnorm = sqrtf(a1 * a1 + a2 * a2);",
        "      float c = a1 / rnorm;",
        "      float s = -a2 / rnorm;",
        "      for (int k = j; k < 8; ++k) {",
        "        float t1 = c * ref[i - 1][k] - s * ref[i][k];",
        "        float t2 = s * ref[i - 1][k] + c * ref[i][k];",
        "        ref[i - 1][k] = t1;",
        "        ref[i][k] = t2;",
        "      }",
        "    }",
        "  }",
        "}",
    ]


def _emit_qr_givens_tolerance_check(lines: list[str]) -> None:
    """Emit tolerance-based OpenHLS output for the QR Givens benchmark.

    Role:
    Compare generated ``r`` against a local software reference and write a stable
    pass/fail line for OpenHLS golden-vs-RTL diffing.

    Goal:
    Match the native OpenHLS QR testbench's ``1e-3f`` tolerance instead of using
    byte checksums that incorrectly require bit-identical floating-point results.
    """

    lines.append("  float ref[8][8];")
    lines.append("  qr_ref(a, ref);")
    lines.append("  const float tol = 1e-3f;")
    lines.append("  for (int i = 0; i < 8; ++i) {")
    lines.append("    for (int j = 0; j < 8; ++j) {")
    lines.append("      float diff = fabsf(r[i][j] - ref[i][j]);")
    lines.append("      if (diff > tol) {")
    lines.append('        fprintf(out, "FAIL qr (%d,%d) got=%f ref=%f diff=%f\\n", i, j, r[i][j], ref[i][j], diff);')
    lines.append("        if (out != stdout) fclose(out);")
    lines.append("        return 1;")
    lines.append("      }")
    lines.append("    }")
    lines.append("  }")
    lines.append('  fprintf(out, "PASS qr_givens\\n");')


def _emit_output_checks(lines: list[str], spec: BenchmarkInputs) -> None:
    """Emit deterministic array checksums for VibeHLS output comparison.

    Role:
    Add FNV-1a byte checksums for every array argument after the kernel call.

    Goal:
    Let OpenHLS compare golden C execution and RTL execution using compact output
    files that still reflect actual memory results, including floating-point bit
    patterns and output arrays.
    """

    arrays = [arg for arg in spec.args if not arg.is_scalar]
    for arg in arrays:
        lines.append(f"  unsigned long long checksum_{arg.name} = 1469598103934665603ull;")
        lines.append(f"  unsigned char *bytes_{arg.name} = (unsigned char *){arg.name};")
        lines.append(f"  for (unsigned long long i = 0; i < sizeof({arg.name}); ++i) {{")
        lines.append(f"    checksum_{arg.name} ^= (unsigned long long)bytes_{arg.name}[i];")
        lines.append(f"    checksum_{arg.name} *= 1099511628211ull;")
        lines.append("  }")
        lines.append(f'  fprintf(out, "{arg.name}_checksum=%llu\\n", checksum_{arg.name});')


def legup_scalar_ties(spec: BenchmarkInputs) -> dict[str, int]:
    """Return scalar argument values keyed by LegUp's ``arg_<name>`` ports."""

    return {f"arg_{arg.name}": int(arg.values[0]) for arg in spec.args if arg.is_scalar}


def legup_memory_regions(spec: BenchmarkInputs) -> dict[str, bytes]:
    """Return packed little-endian memory regions keyed by ``arg_<name>`` ports.

    Role:
    Convert the shared C-level array inputs into the byte contents expected by the
    LegUp RTL memory-controller shim.

    Goal:
    Let LegUp's direct Verilog simulation use the same array contents as Bambu and
    Dynamatic C-driven simulations.
    """

    return {f"arg_{arg.name}": _pack_array(arg) for arg in spec.args if not arg.is_scalar}


def generate_cyclesim_input(spec: BenchmarkInputs) -> dict[str, object]:
    """Serialize shared arguments for OpenHLS compiled CycleSim memory loading.

    Role:
    Convert the common typed fixture into the JSON representation consumed by
    OpenHLS CycleSim, where floating-point elements are IEEE-754 bit patterns
    and integer elements are stored as unsigned machine values.

    Goal:
    Prevent a benchmark-local stale ``cyclesim.input.json`` from diverging from
    the C harness, Vitis, Bambu, and LegUp inputs as fixture sizes evolve.
    """
    scalars = {
        arg.name: int(arg.values[0])
        for arg in spec.args
        if arg.is_scalar
    }
    memories: dict[str, dict[str, object]] = {}
    for arg in spec.args:
        if arg.is_scalar:
            continue
        if arg.c_type == "float":
            data = [struct.unpack("<I", struct.pack("<f", float(value)))[0] for value in arg.values]
        else:
            data = [int(value) for value in arg.values]
        memories[arg.name] = {
            "shape": list(arg.shape),
            "data": data,
            "default": 0,
        }
    return {
        "task": spec.benchmark,
        "scalars": scalars,
        "memories": memories,
        "options": {"max_cycles": 2_000_000},
    }


def _gcd_inputs() -> BenchmarkInputs:
    return BenchmarkInputs(
        benchmark="gcd",
        top_function="gcd",
        return_type="int",
        header=None,
        args=(
            # This pair requires many subtraction-based Euclid iterations,
            # making the loop's scheduled behavior observable in every flow.
            InputObject("a", "int", (), (488732,)),
            InputObject("b", "int", (), (182,)),
        ),
    )


def _dotprod_inputs() -> BenchmarkInputs:
    n = 256
    return BenchmarkInputs(
        benchmark="dotprod",
        top_function="dotprod",
        return_type="int",
        header="dotprod.h",
        args=(
            InputObject("A", "int", (n,), tuple(_init_int_vector(n, 1))),
            InputObject("B", "int", (n,), tuple(_init_int_vector(n, 2))),
        ),
    )


def _matmul_inputs() -> BenchmarkInputs:
    n = 64
    return BenchmarkInputs(
        benchmark="matmul",
        top_function="matmul",
        return_type="void",
        header="matmul.h",
        args=(
            InputObject("A", "int", (n, n), tuple(_init_int_vector(n * n, 1)), is_const=True),
            InputObject("B", "int", (n, n), tuple(_init_int_vector(n * n, 2)), is_const=True),
            InputObject("C", "int", (n, n), tuple(0 for _ in range(n * n)), is_output=True),
        ),
    )


def _spmv_inputs() -> BenchmarkInputs:
    """Return a deterministic 256x256 CSR matrix with variable row lengths.

    Role:
    Define the shared sparse-matrix fixture consumed by every HLS tool runner.

    Goal:
    Exercise steady-state sparse-loop scheduling with 6,554 unique entries,
    the nearest integer to 10% of a 256x256 matrix, rather than repeatedly
    draining a two-entry pipeline for every row.
    """
    n = 256
    nnz = 6554
    selected: set[int] = set()
    state = 0xC0FFEE11
    while len(selected) < nnz:
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        selected.add(state % (n * n))
    row_ptr = [0]
    columns: list[int] = []
    cells = sorted(selected)
    cell_index = 0
    for row in range(n):
        while cell_index < len(cells) and cells[cell_index] // n == row:
            columns.append(cells[cell_index] % n)
            cell_index += 1
        row_ptr.append(len(columns))
    values = tuple(((_mix(1, index + 1) % 200) - 100) / 128.0 for index in range(nnz))
    return BenchmarkInputs(
        benchmark="spmv",
        top_function="spmv",
        return_type="void",
        header=None,
        args=(
            InputObject("val", "float", (nnz,), values),
            InputObject("col", "unsigned char", (nnz,), tuple(columns)),
            InputObject("row_ptr", "unsigned short", (n + 1,), tuple(row_ptr)),
            InputObject("x", "float", (n,), tuple(((_mix(4, i + 5) % 200) - 100) / 256.0 for i in range(n))),
            InputObject("y", "float", (n,), tuple(0.0 for _ in range(n)), is_output=True),
        ),
    )


def _passthrough_inputs() -> BenchmarkInputs:
    """Return shared input/output arrays for the passthrough copy kernel.

    Role:
    Define the 1024-sample integer data set used by every tool runner for the
    passthrough benchmark.

    Goal:
    Keep cycle counts comparable by ensuring LegUp, PANDA/Bambu, Dynamatic, Vitis,
    and OpenHLS all copy the same input array into the same zero-initialized output
    array.
    """

    n = 1024
    return BenchmarkInputs(
        benchmark="passthrough",
        top_function="passthrough",
        return_type="void",
        header="passthrough.h",
        args=(
            InputObject("data_in", "int", (n,), tuple(_init_int_vector(n, 7)), is_const=True),
            InputObject("data_out", "int", (n,), tuple(0 for _ in range(n)), is_output=True),
        ),
    )


def _qr_givens_inputs() -> BenchmarkInputs:
    """Return the deterministic 8x8 matrix used by the QR Givens kernel.

    Role:
    Mirror the benchmark-local software harness's well-conditioned floating-point
    matrix so every tool applies the same sequence of Givens rotations.

    Goal:
    Keep floating-point cycle comparisons tied to the intended non-triangular
    workload instead of an arbitrary or tool-specific matrix.
    """

    n = 8
    values: list[float] = []
    for i in range(n):
        for j in range(n):
            value = ((i * 7 + j * 13) % 17) - 8
            if i == j:
                value += 4
            if i == 0 or j == n - 1:
                value += 2
            values.append(float(value))
    return BenchmarkInputs(
        benchmark="qr_givens",
        top_function="qr_givens",
        return_type="void",
        header="qr.h",
        args=(
            InputObject("a", "float", (n, n), tuple(values), is_const=True),
            InputObject("r", "float", (n, n), tuple(0.0 for _ in range(n * n)), is_output=True),
        ),
    )


def _lbm_inputs() -> BenchmarkInputs:
    """Return the deterministic D2Q9 lattice used by the LBM benchmark.

    Role:
    Reproduce the benchmark-local LCG initialization and relaxation factor for
    the in-place distribution-function array.

    Goal:
    Let all simulators exercise the same 32x32 floating-point collision step
    while preserving that ``f`` is both an input and an output argument.
    """

    nx = 32
    ny = 32
    q = 9
    state = 109
    values: list[float] = []
    for _ in range(nx * ny * q):
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        values.append(0.1 + ((state & 0xFFFF) / 65535.0) * 0.1)
    return BenchmarkInputs(
        benchmark="lbm",
        top_function="lbm",
        return_type="void",
        header="lbm.h",
        args=(
            InputObject("f", "float", (nx, ny, q), tuple(values), is_output=True),
            InputObject("omega", "float", (), (1.0,)),
        ),
    )


def _histogram_inputs() -> BenchmarkInputs:
    """Return deterministic byte samples and zeroed bins for ``histogram``.

    Role:
    Match the benchmark-local byte LCG so the eight histogram bins observe the
    same 64 input samples in C, RTL, and generated memory-model simulations.

    Goal:
    Exercise the read/modify/write bin dependency without changing the benchmark
    problem size or data distribution.
    """

    hist_n = 64
    bins = 8
    state = 47
    samples: list[int] = []
    for _ in range(hist_n):
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        samples.append(state & 0xFF)
    return BenchmarkInputs(
        benchmark="histogram",
        top_function="histogram",
        return_type="void",
        header=None,
        args=(
            InputObject("in", "uint8_t", (hist_n,), tuple(samples)),
            InputObject("bins", "uint16_t", (bins,), tuple(0 for _ in range(bins)), is_output=True),
        ),
    )


def _iir_inputs() -> BenchmarkInputs:
    """Return bounded integer streams for the IIR recurrence benchmark.

    Role:
    Provide an initialized feedback array, an input sample stream, and scalar
    coefficients for the 1000-iteration recurrence in ``iir.c``.

    Goal:
    Keep the recurrence dynamic and deterministic while avoiding coefficient
    choices that would overflow 32-bit integers immediately and obscure tool
    behavior.
    """

    n = 1000
    y_values = [0 for _ in range(n)]
    y_values[0] = 3
    x_values = list(_init_int_vector(n, 11))
    return BenchmarkInputs(
        benchmark="iir",
        top_function="iir",
        return_type="int",
        header="iir.h",
        args=(
            InputObject("y", "in_int_t", (n,), tuple(y_values), is_output=True),
            InputObject("x", "in_int_t", (n,), tuple(x_values)),
            InputObject("a", "in_int_t", (), (1,)),
            InputObject("b", "in_int_t", (), (2,)),
        ),
    )


def _iterative_sqrt_inputs() -> BenchmarkInputs:
    """Return the scalar input used by generated harnesses for iterative_sqrt.

    Role:
    Provide one deterministic non-square input for the scalar integer square-root
    benchmark when a runner generates its own main function.

    Goal:
    Keep generated-harness simulations comparable across tools while the
    benchmark-local testbench still exercises a broader set of scalar cases.
    """

    return BenchmarkInputs(
        benchmark="iterative_sqrt",
        top_function="iterative_sqrt",
        return_type="int",
        header="iterative_sqrt.h",
        args=(
            InputObject("n", "in_int_t", (), (1023,)),
        ),
    )


def _rv32i_comet_inputs() -> BenchmarkInputs:
    """Return the RISC-V program/data memories used by ``rv32i_comet``.

    Role:
    Materialize the same encoded instruction stream as the benchmark-local C++
    harness without relying on host-side encoder helper functions.

    Goal:
    Keep generated C/C++ testbenches simple while still running the intended loop,
    store, load, and final check sequence through the processor model.
    """

    imem_words = 64
    dmem_words = 64
    instr = [0 for _ in range(imem_words)]
    instr[0] = _rv32i_enc_addi(1, 0, 5)
    instr[1] = _rv32i_enc_addi(2, 0, 0)
    instr[2] = _rv32i_enc_addi(2, 2, 3)
    instr[3] = _rv32i_enc_addi(1, 1, -1)
    instr[4] = _rv32i_enc_bne(1, 0, -8)
    instr[5] = _rv32i_enc_sw(2, 0, 0)
    instr[6] = _rv32i_enc_lw(3, 0, 0)
    instr[7] = _rv32i_enc_addi(4, 0, 15)
    instr[8] = _rv32i_enc_sub(5, 3, 4)
    instr[9] = _rv32i_enc_sw(5, 0, 4)
    instr[10] = 0
    return BenchmarkInputs(
        benchmark="rv32i_comet",
        top_function="rv32i_comet_run",
        return_type="int",
        header="rv32i_comet.h",
        args=(
            InputObject("instr_mem", "uint32_t", (imem_words,), tuple(instr)),
            InputObject("data_mem", "uint32_t", (dmem_words,), tuple(0 for _ in range(dmem_words)), is_output=True),
        ),
    )


def _rv32i_truecomet_inputs() -> BenchmarkInputs:
    """Return the upstream true-Comet arithmetic-and-memory program fixture.

    Role:
    Define the exact two-array ABI and directed program used for cross-tool
    TrueComet execution.

    Goal:
    Exercise the upstream harness's arithmetic, forwarding, and byte-addressed
    memory path with the public kernel ABI: 64 instruction words and one
    zero-initialized 256-byte data memory.  This matches the C header and avoids
    fabricating four lane-pointer arguments that the kernel does not accept.
    """

    imem_words = 64
    dmem_words = 64
    instr = [0 for _ in range(imem_words)]
    instr[:35] = (
        0x112230B7, 0x34408093, 0x00102023, 0x00000103, 0x00304183,
        0x00001203, 0x00205283, 0x00300223, 0x00401323, 0x00310333,
        0x402183B3, 0x00219433, 0x003124B3, 0x0021B533, 0x003145B3,
        0x0020D633, 0x4020D6B3, 0x00216733, 0x003177B3, 0x00119813,
        0x0040D893, 0x4040D913, 0x10012993, 0x10013A13, 0x07F14A93,
        0x08016B13, 0x07F17B93, 0x0000000F, 0x00602423, 0x00702623,
        0x00802823, 0x00D02A23, 0x01002C23, 0x01202E23, 0x00000073,
    )
    return BenchmarkInputs(
        benchmark="rv32i_truecomet",
        top_function="rv32i_true_comet_run",
        return_type="int",
        header="rv32i_true_comet.h",
        args=(
            InputObject("instr_mem", "uint32_t", (imem_words,), tuple(instr)),
            InputObject("data_mem", "uint8_t", (dmem_words * 4,), tuple(0 for _ in range(dmem_words * 4)), is_output=True),
        ),
    )


def _rv32i_inputs() -> BenchmarkInputs:
    """Return the RV32I_MINI GCD image in the interpreter's unusual RAM layout.

    Role:
    Materialize the same little-endian image defined by ``gcd_image.c`` for flows
    that need explicit pointer-argument memory contents, including LegUp RTL
    simulation.

    Goal:
    Ensure the processor executes the GCD workload rather than an all-zero
    instruction RAM.  The interpreter addresses ``instr_mem`` with a byte PC,
    while each data-memory byte lane is addressed by word index; both layouts are
    reproduced exactly here.
    """

    # Keep this image byte-for-byte aligned with rv32i/src/gcd_image.c.  The
    # interpreter indexes instruction memory with a byte PC, while its four data
    # lanes use word indices; the later packing loop reproduces both conventions.
    program_words = (
        0x20002283, 0x20402303, 0x00100393, 0x0062CC63,
        0x02534663, 0x20502423, 0x00100493, 0x20902623,
        0x00000000, 0x00028413, 0x00804463, 0xFE7040E3,
        0xFFF30313, 0xFFF40413, 0xFE7048E3, 0x00030413,
        0x00804463, 0xFC7044E3, 0xFFF28293, 0xFFF40413,
        0xFE7048E3,
    ) + (0,) * 107 + (21, 15, 0, 0)
    assert len(program_words) == 132

    memory_words = 1024
    instr_mem = [0] * memory_words
    lanes = [[0] * memory_words for _ in range(4)]
    for word_index, word in enumerate(program_words):
        # cpu_run indexes instructions by byte PC, unlike its word-indexed lanes.
        instr_mem[word_index * 4] = word
        for lane_index, lane in enumerate(lanes):
            lane[word_index] = (word >> (lane_index * 8)) & 0xFF

    return BenchmarkInputs(
        benchmark="rv32i",
        top_function="cpu_run",
        return_type="int",
        header="rv32i.h",
        args=(
            InputObject("init_pc", "uint32_t", (), (0,)),
            InputObject("instr_mem", "uint32_t", (memory_words,), tuple(instr_mem)),
            InputObject("mem0", "uint8_t", (memory_words,), tuple(lanes[0]), is_output=True),
            InputObject("mem1", "uint8_t", (memory_words,), tuple(lanes[1]), is_output=True),
            InputObject("mem2", "uint8_t", (memory_words,), tuple(lanes[2]), is_output=True),
            InputObject("mem3", "uint8_t", (memory_words,), tuple(lanes[3]), is_output=True),
        ),
    )


def _init_int_vector(count: int, seed: int) -> Iterable[int]:
    for _ in range(count):
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        yield int((seed >> 8) % 17) - 8


def _mix(seed: int, salt: int) -> int:
    return (seed * 1664525 + 1013904223 + salt * 97) & 0xFFFFFFFF


def _rv32i_enc_addi(rd: int, rs1: int, imm: int) -> int:
    return (((imm & 0xFFF) << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x13) & 0xFFFFFFFF


def _rv32i_enc_sub(rd: int, rs1: int, rs2: int) -> int:
    return ((0x20 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x33) & 0xFFFFFFFF


def _rv32i_enc_lw(rd: int, rs1: int, imm: int) -> int:
    return (((imm & 0xFFF) << 20) | (rs1 << 15) | (0x2 << 12) | (rd << 7) | 0x03) & 0xFFFFFFFF


def _rv32i_enc_sw(rs2: int, rs1: int, imm: int) -> int:
    uimm = imm & 0xFFF
    return ((((uimm >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (0x2 << 12) | ((uimm & 0x1F) << 7) | 0x23) & 0xFFFFFFFF


def _rv32i_enc_bne(rs1: int, rs2: int, imm: int) -> int:
    uimm = imm & 0x1FFF
    return (((uimm >> 12) & 0x1) << 31 | (((uimm >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) | (0x1 << 12) | (((uimm >> 1) & 0x0F) << 8) | (((uimm >> 11) & 0x1) << 7) | 0x63) & 0xFFFFFFFF


def _prototype_arg(arg: InputObject) -> str:
    """Render one kernel argument for generated testbench prototypes.

    Role:
    Provide explicit callable prototypes when a tool harness includes benchmark
    headers that may only define constants and typedefs.

    Goal:
    Match the canonical benchmark function declarations exactly enough for strict
    C frontends.  Constness is carried explicitly in the shared input metadata:
    adding ``const`` to every read-only generated array breaks headers such as
    ``iir.h``, while omitting it breaks headers such as ``qr.h``.
    """

    if arg.is_scalar:
        return f"{arg.c_type} {arg.name}"
    dims = "".join(f"[{dim}]" for dim in arg.shape)
    qualifier = "const " if arg.is_const else ""
    return f"{qualifier}{arg.c_type} {arg.name}{dims}"


def _emit_c_array(arg: InputObject) -> list[str]:
    dims = "".join(f"[{dim}]" for dim in arg.shape)
    flat = ", ".join(_c_literal(arg.c_type, value) for value in arg.values)
    return [
        f"static const {arg.c_type} init_{arg.name}_data[{arg.element_count}] = {{{flat}}};",
        f"static void init_{arg.name}({arg.c_type} {arg.name}{dims}) {{",
        f"  {arg.c_type} *flat = ({arg.c_type} *){arg.name};",
        f"  for (int i = 0; i < {arg.element_count}; ++i) flat[i] = init_{arg.name}_data[i];",
        "}",
    ]


def _c_literal(c_type: str, value: int | float) -> str:
    """Render one C literal for generated cross-tool testbenches.

    Role:
    Convert Python scalar values from the shared benchmark specification into
    syntax accepted by the C compilers used by Bambu and Dynamatic simulation.

    Goal:
    Preserve single-precision intent for float arrays while avoiding malformed
    literals such as ``0f``, which C parses as an invalid integer suffix.
    """

    if c_type == "float":
        literal = f"{float(value):.9g}"
        if "e" not in literal.lower() and "." not in literal:
            literal += ".0"
        return f"{literal}f"
    if c_type in {"uint8_t", "uint16_t", "uint32_t"}:
        return f"({c_type}){int(value) & 0xFFFFFFFF}u"
    return str(int(value))


def _pack_array(arg: InputObject) -> bytes:
    out = bytearray()
    for value in arg.values:
        if arg.c_type == "float":
            out.extend(struct.pack("<f", float(value)))
        elif arg.c_type in {"unsigned char", "uint8_t"}:
            out.extend(struct.pack("<B", int(value) & 0xFF))
        elif arg.c_type in {"uint16_t", "unsigned short"}:
            out.extend(struct.pack("<H", int(value) & 0xFFFF))
        elif arg.c_type in {"uint32_t", "unsigned int"}:
            out.extend(struct.pack("<I", int(value) & 0xFFFFFFFF))
        else:
            out.extend(struct.pack("<i", int(value)))
    return bytes(out)
