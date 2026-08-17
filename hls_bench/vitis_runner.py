"""Run local Vitis HLS on repository benchmarks.

Role:
Discover benchmark kernels under ``./benchmarks``, stage each kernel with the
shared deterministic C testbench inputs, run Vitis HLS synthesis and optional RTL
co-simulation, and emit native plus normalized QoR summaries.

Goal:
Add Vitis HLS as a comparable tool in the existing LegUp/Bambu/Dynamatic sweep
without changing benchmark inputs or replacing exact cycle counts with scheduling
estimates.  Latency is populated only from Vitis RTL co-simulation reports.

How:
For each selected benchmark the runner writes an isolated project directory under
``artifacts/tool-work/vitis/<benchmark>``, creates a Tcl script invoking ``csynth_design``
and optionally ``cosim_design -rtl verilog -tool xsim``, then parses Vitis HLS
``csynth.xml`` and co-simulation reports into the common QoR schema.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
from typing import Any
import xml.etree.ElementTree as ET

from hls_bench import inputs as common_inputs
from hls_bench import loop_labels
from hls_bench import paths
from hls_bench import qor as common_qor


REPO_ROOT = Path(__file__).resolve().parent.parent
BENCHMARKS_DIR = paths.BENCHMARKS_ROOT
DEFAULT_WORK_ROOT = paths.VITIS_WORK_ROOT
DEFAULT_SUMMARY = DEFAULT_WORK_ROOT / "vitis_run_all.json"
DEFAULT_NORMALIZED_SUMMARY = DEFAULT_WORK_ROOT / "vitis_qor.json"
DEFAULT_CLOCK_PERIOD_NS = 10.0
DEFAULT_PART = "xc7a100tcsg324-1"
DEFAULT_VITIS_HLS = "/opt/Xilinx/Xilinx/Vitis_HLS/2023.2/bin/vitis_hls"
DEFAULT_VIVADO = "/opt/Xilinx/Xilinx/Vivado/2023.2/bin/vivado"
CRT_LIBRARY_PATH = Path("/usr/lib/x86_64-linux-gnu")
# RV32I exposes byte-addressed 16 MiB memories through 32-bit instruction words
# and byte-lane data arrays, so every top-level pointer spans 2^22 entries.
RV32I_MEMORY_ELEMENTS = 1 << 22


@dataclass(frozen=True)
class VitisJob:
    """One benchmark prepared for Vitis HLS execution.

    Role:
    Carry immutable source, top-function, staging, and output paths from
    discovery into subprocess execution.

    Goal:
    Keep benchmark discovery deterministic and make failed/skipped jobs report the
    same path fields as successful jobs.
    """

    name: str
    benchmark_dir: Path
    source: Path
    top_function: str
    source_dir: Path
    project_dir: Path
    staged_source: Path
    testbench_source: Path
    extra_testbench_sources: tuple[Path, ...]
    script: Path
    log: Path
    vivado_dir: Path
    vivado_script: Path
    vivado_log: Path


@dataclass
class VitisResult:
    """Native execution summary for one Vitis HLS benchmark.

    Role:
    Store process status, command, elapsed time, and artifact locations before
    conversion to the normalized common QoR schema.

    Goal:
    Preserve enough information to reproduce failures while keeping the common
    report schema independent from Vitis-specific directory names.
    """

    name: str
    status: str
    returncode: int | None
    elapsed_seconds: float | None
    source: str | None
    top_function: str | None
    command: list[str]
    log_path: str | None
    project_dir: str | None
    script_path: str | None
    csynth_xml: str | None = None
    cosim_report: str | None = None
    vivado_util_report: str | None = None
    vivado_timing_report: str | None = None
    vivado_log_path: str | None = None
    message: str | None = None


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse Vitis HLS runner options.

    Role:
    Define benchmark selection, executable path, target clock/part, execution
    toggles, timeout, and output report paths for the top-level CLI.

    Goal:
    Match the ergonomics of the existing tool runners while keeping Vitis HLS
    optional and explicit in parallel sweeps.
    """

    parser = argparse.ArgumentParser(description="Run local Vitis HLS over benchmark kernels.", allow_abbrev=False)
    parser.add_argument("--benchmarks", nargs="+", help="benchmark directory names to run")
    parser.add_argument("--bench", action="append", help="single benchmark name; may be repeated")
    parser.add_argument("--vitis-hls", default=os.environ.get("VITIS_HLS", DEFAULT_VITIS_HLS), help="vitis_hls executable")
    parser.add_argument("--vivado", default=os.environ.get("VIVADO", DEFAULT_VIVADO), help="Vivado executable for post-HLS synthesis")
    parser.add_argument("--work-root", type=Path, default=DEFAULT_WORK_ROOT)
    parser.add_argument("--summary", type=Path, default=DEFAULT_SUMMARY)
    parser.add_argument("--normalized-summary", type=Path, default=DEFAULT_NORMALIZED_SUMMARY)
    parser.add_argument("--clock-period", type=float, default=DEFAULT_CLOCK_PERIOD_NS)
    parser.add_argument("--part", default=DEFAULT_PART)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--run-synthesis", action="store_true", help="run csynth_design and post-HLS Vivado synthesis")
    parser.add_argument("--run-simulation", action="store_true", help="run RTL co-simulation and parse exact cycles")
    parser.add_argument(
        "--unsafe-math-optimizations",
        action="store_true",
        help=(
            "enable Vitis associative floating-point optimizations; this relaxes "
            "signed-zero semantics and can change numerical results"
        ),
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Run the Vitis HLS batch command and return a process exit code."""

    args = parse_args(argv)
    results = run(args)
    failed = any(result.status in {"FAIL", "TIMEOUT", "SKIP"} for result in results)
    return 1 if failed else 0


def run(args: argparse.Namespace) -> list[VitisResult]:
    """Discover, run, report, and print a compact Vitis HLS batch summary.

    Role:
    Coordinate executable validation, benchmark discovery, staging, execution,
    native JSON writing, normalized JSON writing, and console table output.

    Goal:
    Provide one complete end-to-end entry point usable from both ``hls-bench.py
    vitis`` and the parallel scheduler.
    """

    selected = _selected_benchmarks(args)
    jobs, results = discover_jobs(args.work_root, selected)
    vitis_error = _validate_vitis_hls(args.vitis_hls)
    if vitis_error is not None:
        results.extend(_skipped_result(job.name, vitis_error, job) for job in jobs)
        jobs = []
    if jobs and args.run_synthesis:
        vivado_error = _validate_vivado(args.vivado)
        if vivado_error is not None:
            results.extend(_skipped_result(job.name, vivado_error, job) for job in jobs)
            jobs = []
    for job in jobs:
        results.append(run_job(job, args))
    write_native_summary(args.summary, args, results)
    write_normalized_summary(args.normalized_summary, args, results)
    print_table(results)
    print(f"\nsummary: {args.summary}")
    print(f"normalized QoR: {args.normalized_summary}")
    return results


def discover_jobs(work_root: Path, selected: set[str] | None) -> tuple[list[VitisJob], list[VitisResult]]:
    """Discover supported benchmark kernels and stage source/testbench files.

    Role:
    Choose one C/C++ kernel source per selected benchmark, infer or reuse the top
    function name, copy headers, and write a shared-input Vitis C testbench.

    Goal:
    Ensure Vitis HLS sees the same deterministic inputs as the other tool flows
    without mutating files under ``./benchmarks``.
    """

    jobs: list[VitisJob] = []
    skipped: list[VitisResult] = []
    for benchmark_dir in sorted(path for path in BENCHMARKS_DIR.iterdir() if path.is_dir()):
        if selected is not None and benchmark_dir.name not in selected:
            continue
        source_root = benchmark_dir / "src" if (benchmark_dir / "src").is_dir() else benchmark_dir
        source = select_source(source_root)
        if source is None:
            skipped.append(_bare_skipped(benchmark_dir.name, "could not identify a unique C/C++ kernel source"))
            continue
        shared = common_inputs.get_benchmark_inputs(benchmark_dir.name)
        top_function = shared.top_function if shared is not None else infer_top_function(source)
        if top_function is None:
            skipped.append(_bare_skipped(benchmark_dir.name, "could not infer top function"))
            continue
        if shared is None and benchmark_dir.name != "rv32i":
            skipped.append(_bare_skipped(benchmark_dir.name, "no shared deterministic input specification"))
            continue

        bench_root = work_root / benchmark_dir.name
        source_dir = bench_root / "src"
        project_dir = bench_root / "project"
        if bench_root.exists():
            shutil.rmtree(bench_root)
        source_dir.mkdir(parents=True, exist_ok=True)
        staged_source = source_dir / source.name
        translate_source_for_vitis(source, staged_source)
        copy_headers(source_root, source_dir)
        extra_testbench_sources: tuple[Path, ...] = ()
        # RV32I needs its benchmark-local 16 MiB memory image and GCD assertion;
        # the generic shared harness intentionally models only small fixed arrays.
        if benchmark_dir.name == "rv32i":
            testbench_source, extra_testbench_sources = stage_rv32i_testbench(source_root, source_dir)
        else:
            testbench_suffix = ".cpp" if source.suffix in {".cpp", ".cc", ".cxx"} else ".c"
            testbench_source = source_dir / f"tb{testbench_suffix}"
            testbench_source.write_text(vitis_testbench(common_inputs.generate_c_harness(shared, use_dynamatic_call=False)), encoding="utf-8")
        jobs.append(
            VitisJob(
                name=benchmark_dir.name,
                benchmark_dir=benchmark_dir,
                source=source,
                top_function=top_function,
                source_dir=source_dir,
                project_dir=project_dir,
                staged_source=staged_source,
                testbench_source=testbench_source,
                extra_testbench_sources=extra_testbench_sources,
                script=bench_root / "run_vitis_hls.tcl",
                log=bench_root / "vitis_hls.log",
                vivado_dir=bench_root / "vivado_synth",
                vivado_script=bench_root / "vivado_synth" / "synthesize.tcl",
                vivado_log=bench_root / "vivado_synth" / "vivado.log",
            )
        )
    return jobs, skipped


def stage_rv32i_testbench(source_root: Path, source_dir: Path) -> tuple[Path, tuple[Path, ...]]:
    """Stage the benchmark-local deterministic RV32I GCD testbench for Vitis.

    Role:
    Copy ``main_rv32i.cpp`` and the embedded GCD image source beside the staged
    Vitis kernel source.

    Goal:
    Run the same deterministic GCD program used by the benchmark-local testbench
    without trying to synthesize a huge generated initializer in
    ``hls_bench.inputs``.  The extra ``gcd_image.c`` testbench source provides the
    ``gcd_bin`` and ``gcd_bin_len`` symbols consumed by ``main_rv32i.cpp``.
    """

    main_source = source_root / "main_rv32i.cpp"
    gcd_source = source_root / "gcd_image.c"
    if not main_source.is_file() or not gcd_source.is_file():
        missing = [str(path) for path in (main_source, gcd_source) if not path.is_file()]
        raise FileNotFoundError("missing rv32i Vitis testbench source(s): " + ", ".join(missing))
    staged_main = source_dir / main_source.name
    staged_gcd = source_dir / gcd_source.name
    shutil.copy2(main_source, staged_main)
    shutil.copy2(gcd_source, staged_gcd)
    return staged_main, (staged_gcd,)


def run_job(job: VitisJob, args: argparse.Namespace) -> VitisResult:
    """Run one Vitis HLS job and collect its native result.

    Role:
    Write the Tcl script, invoke ``vitis_hls``, enforce the timeout, and record
    report paths when the tool finishes.

    Goal:
    Treat failed synthesis/co-simulation as visible job failures rather than
    silently falling back to estimates or stale reports.
    """

    run_synthesis = args.run_synthesis or args.run_simulation
    write_tcl(job, args, run_synthesis=run_synthesis, run_simulation=args.run_simulation)
    command = [args.vitis_hls, str(job.script)]
    if args.dry_run:
        return VitisResult(job.name, "DRY-RUN", None, None, str(job.source), job.top_function, command, str(job.log), str(job.project_dir), str(job.script))
    started = time.monotonic()
    env = vitis_environment()
    with job.log.open("w", encoding="utf-8") as log:
        try:
            proc = subprocess.run(command, cwd=job.project_dir.parent, stdout=log, stderr=subprocess.STDOUT, text=True, timeout=args.timeout, env=env)
            returncode = proc.returncode
            timed_out = False
        except subprocess.TimeoutExpired:
            returncode = None
            timed_out = True
    elapsed = time.monotonic() - started
    csynth_xml = csynth_xml_path(job)
    cosim_report = find_cosim_report(job)
    vivado_returncode: int | None = None
    vivado_message: str | None = None
    if run_synthesis and csynth_xml.is_file():
        vivado_returncode, vivado_message = run_vivado_synthesis(job, args)
    if timed_out:
        status = "TIMEOUT"
        message = f"Vitis HLS timed out after {args.timeout:g}s"
    elif returncode == 0:
        status = "PASS"
        message = None
    else:
        status = "FAIL"
        message = f"vitis_hls failed with exit code {returncode}"
    if args.run_simulation and returncode == 0 and parse_vitis_cosim_cycles(cosim_report) is None:
        status = "FAIL"
        message = "Vitis RTL co-simulation completed without a parseable cycle count"
    if status == "PASS" and vivado_returncode not in {None, 0}:
        status = "FAIL"
        message = vivado_message
    elif status == "PASS" and run_synthesis and not vivado_reports_exist(job):
        status = "FAIL"
        message = "Vivado post-HLS synthesis completed without reports"
    return VitisResult(
        name=job.name,
        status=status,
        returncode=returncode,
        elapsed_seconds=elapsed,
        source=str(job.source),
        top_function=job.top_function,
        command=command,
        log_path=str(job.log),
        project_dir=str(job.project_dir),
        script_path=str(job.script),
        csynth_xml=str(csynth_xml) if csynth_xml.is_file() else None,
        cosim_report=str(cosim_report) if cosim_report and cosim_report.is_file() else None,
        vivado_util_report=str(vivado_util_report(job)) if vivado_util_report(job).is_file() else None,
        vivado_timing_report=str(vivado_timing_report(job)) if vivado_timing_report(job).is_file() else None,
        vivado_log_path=str(job.vivado_log) if job.vivado_log.is_file() else None,
        message=message,
    )


def write_tcl(job: VitisJob, args: argparse.Namespace, *, run_synthesis: bool, run_simulation: bool) -> None:
    """Write the Vitis HLS Tcl script for one benchmark.

    Role:
    Encode project setup, source/testbench registration, part/clock selection,
    synthesis, and optional RTL co-simulation in a reproducible script artifact.

    Goal:
    Keep every Vitis job self-contained and directly rerunnable from its work
    directory for debugging.
    """

    job.project_dir.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        f"open_project -reset {{{job.project_dir}}}",
        f"set_top {job.top_function}",
        f"add_files {{{job.staged_source}}}",
        f"add_files -tb {{{job.testbench_source}}}",
        *[f"add_files -tb {{{source}}}" for source in job.extra_testbench_sources],
        "open_solution -reset solution1 -flow_target vivado",
        f"set_part {{{args.part}}}",
        f"create_clock -period {args.clock_period:g} -name default",
        "config_unroll -tripcount_threshold 0",
    ]
    if args.unsafe_math_optimizations:
        # Vitis performs associative floating-point rewrites only with this
        # explicit opt-in. Keep it before synthesis so the compiler, rather
        # than a loop-II directive, decides whether a reduction can be pipelined.
        lines.append("config_compile -unsafe_math_optimizations")
    lines.extend(vitis_directives(job))
    if run_synthesis:
        lines.append("csynth_design")
    if run_simulation:
        lines.append("cosim_design -rtl verilog -tool xsim")
    lines.append("exit")
    job.script.write_text("\n".join(lines) + "\n", encoding="utf-8")


def vitis_directives(job: VitisJob) -> list[str]:
    """Build Vitis Tcl directives derived from benchmark-local HLS pragmas.

    Role:
    Translate the subset of source pragmas that affect scheduler fairness across
    tools into Vitis-compatible Tcl directives after the solution is opened.

    Goal:
    Keep Vitis constrained by the same explicit memory-port intent as the other
    flows and prevent Vitis from silently applying loop unrolling on labeled
    pipelined loops while avoiding hard-coded benchmark-specific directives.
    """

    directives: list[str] = []
    directives.extend(vitis_interface_directives(job))
    for directive in vitis_bind_storage_directives(job):
        directives.append(directive)
    for loop_label, ii, _, _ in pipeline_loops_for_vitis(job.source):
        command = f"set_directive_pipeline {{{job.top_function}/{loop_label}}}"
        if ii is not None:
            command += f" -II {ii}"
        directives.append(command)
        directives.append(f"set_directive_unroll {{{job.top_function}/{loop_label}}} -off")
    return directives


def vitis_interface_directives(job: VitisJob) -> list[str]:
    """Return one-port Vitis BRAM directives for shared array arguments.

    Role:
    Derive external-memory directives from the shared benchmark input manifest,
    including the array bounds needed by RV32I C/RTL co-simulation.

    Goal:
    Give every tool the same one-read/one-write external-BRAM contract.  Vitis
    otherwise infers extra memory ports for independently accessed array values,
    making its pipeline II and RTL cycle count incomparable with OpenHLS.

    How:
    Apply ``ram_1p`` storage to each non-scalar manifest argument.  RV32I also
    retains its explicit 16 MiB depth because Vitis cannot infer that bound from
    its pointer ABI during generated testbench execution.
    """

    spec = common_inputs.get_benchmark_inputs(job.name)
    if spec is None:
        return []
    directives: list[str] = []
    for argument in spec.args:
        if argument.is_scalar:
            continue
        command = "set_directive_interface -mode ap_memory -storage_type ram_1p"
        if job.name == "rv32i":
            command += f" -depth {RV32I_MEMORY_ELEMENTS}"
        directives.append(f"{command} {{{job.top_function}}} {argument.name}")
    return directives


def vitis_bind_storage_directives(job: VitisJob) -> list[str]:
    """Translate benchmark ``resource`` pragmas into Vitis storage bindings.

    Role:
    Parse only source-level ``#pragma HLS resource`` lines and convert their
    ``variable``, ``core``, and optional ``latency`` fields into
    ``set_directive_bind_storage`` Tcl commands understood by Vitis HLS.

    Goal:
    Preserve explicit benchmark memory-port constraints such as ``ram_1p`` for
    Vitis instead of letting the staging step strip those pragmas and allowing
    Vitis to infer wider memories than the benchmark requested.
    """

    directives: list[str] = []
    for line in source_resource_lines(job):
        if not re.match(r"\s*#\s*pragma\s+HLS\s+resource\b", line, re.IGNORECASE):
            continue
        fields = dict(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^\s]+)", line))
        variable = fields.get("variable")
        core = fields.get("core")
        if variable is None or core is None:
            continue
        command = f"set_directive_bind_storage {{{job.top_function}}} {variable} -type {core}"
        latency = fields.get("latency")
        # OpenHLS pragmas may use latency=0 for same-cycle memory intent, but
        # Vitis bind_storage only accepts positive implementation latencies.
        if latency is not None and latency != "0":
            command += f" -latency {latency}"
        directives.append(command)
    return directives


def source_resource_lines(job: VitisJob) -> list[str]:
    """Return source lines that may contain benchmark resource pragmas.

    Role:
    Prefer resource pragmas from the canonical benchmark source and fall back to
    previously staged OpenHLS copies when the canonical source has lost those
    constraints but a matching generated benchmark copy still records them.

    Goal:
    Keep Vitis constrained by the same benchmark memory intent as OpenHLS without
    hard-coding individual variable names in the Vitis runner.
    """

    candidates = [
        job.source,
        paths.OPENHLS_WORK_ROOT / job.name / job.source.name,
        paths.TOOL_WORK_ROOT / "openhls_legacy" / job.name / job.source.name,
    ]
    for candidate in candidates:
        if not candidate.is_file():
            continue
        lines = candidate.read_text(encoding="utf-8", errors="replace").splitlines()
        if any(re.match(r"\s*#\s*pragma\s+HLS\s+resource\b", line, re.IGNORECASE) for line in lines):
            return lines
    return job.source.read_text(encoding="utf-8", errors="replace").splitlines()


def pipeline_loops_for_vitis(source: Path) -> list[tuple[str, int | None, int, bool]]:
    """Return labels and II values for source pipeline directives.

    Role:
    Find existing C labels immediately associated with nearby ``#pragma HLS
    pipeline`` directives and assign deterministic labels to unlabeled pipeline
    pragmas so generated Vitis Tcl can address every explicit pipeline loop.

    Goal:
    Preserve explicitly requested pipelining while disabling unrolling on those
    same loops, without depending on Vitis-generated loop names or applying broad
    directives to unrelated code.
    """

    loops: list[tuple[str, int | None, int, bool]] = []
    lines = source.read_text(encoding="utf-8", errors="replace").splitlines()
    labeled = loop_labels.label_c_source_text("\n".join(lines) + "\n")[1]
    label_by_line = {entry.line_index: entry.label for entry in labeled}
    pipeline_re = re.compile(r"\s*#\s*pragma\s+HLS\s+pipeline\b", re.IGNORECASE)
    for index, line in enumerate(lines):
        if not pipeline_re.match(line):
            continue
        ii_match = re.search(r"\bii\s*=\s*(\d+)", line, re.IGNORECASE)
        ii = int(ii_match.group(1)) if ii_match else None
        loop_index = next((entry.line_index for entry in labeled if entry.line_index > index), None)
        if loop_index is None:
            continue
        loops.append((label_by_line.get(loop_index, f"vitis_pipeline_{len(loops)}"), ii, index, False))
    return loops


def select_source(benchmark_dir: Path) -> Path | None:
    """Select the most likely synthesizable source file in a benchmark directory."""

    candidates = sorted(
        path
        for suffix in ("*.c", "*.cc", "*.cpp", "*.cxx")
        for path in benchmark_dir.glob(suffix)
        if not path.name.startswith("main") and "trace_stub" not in path.stem and "_ref" not in path.stem
    )
    kernel_candidates = [path for path in candidates if "kernel" in path.stem]
    if len(kernel_candidates) == 1:
        return kernel_candidates[0]
    if len(candidates) == 1:
        return candidates[0]
    named = benchmark_dir / f"{benchmark_dir.name}.c"
    if named.is_file():
        return named
    return None


def infer_top_function(source: Path) -> str | None:
    """Infer a top function from HLS annotations or the first external function."""

    text = source.read_text(encoding="utf-8", errors="replace")
    annotated = re.search(r'__attribute__\s*\(\(annotate\("hls\.toplevel"\)\)\)\s*(?:void|int|float|double|char|short|long|unsigned|signed|uint\d+_t|int\d+_t)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(', text)
    if annotated:
        return annotated.group(1)
    lines = text.splitlines()
    func_re = re.compile(r"\b(?:void|int|float|double|char|short|long|unsigned|signed|uint\d+_t|int\d+_t)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    for index, line in enumerate(lines):
        if "#pragma HLS top" not in line:
            continue
        for next_line in lines[index : min(index + 40, len(lines))]:
            match = func_re.search(next_line)
            if match:
                return match.group(1)
    for line in lines:
        match = func_re.search(line)
        if match and match.group(1) not in {"main", "printf"}:
            return match.group(1)
    return None


def copy_headers(benchmark_dir: Path, source_dir: Path) -> None:
    """Copy local headers beside the staged Vitis source."""

    for pattern in ("*.h", "*.hpp"):
        for header in sorted(benchmark_dir.glob(pattern)):
            shutil.copy2(header, source_dir / header.name)


def translate_source_for_vitis(source: Path, staged_source: Path) -> None:
    """Write a Vitis-compatible staged kernel source copy.

    Role:
    Remove benchmark-local HLS pragmas that Vitis 2023.2 rejects at file scope
    and host-only ``stdio`` tracing that APCC cannot compile on this Debian host,
    while leaving functional C/C++ computation and in-function scheduling pragmas
    intact.

    Goal:
    Let the runner provide top-function selection through Tcl ``set_top`` without
    mutating ``./benchmarks`` or hiding real kernel computation failures.
    """

    filtered: list[str] = []
    source_text = source.read_text(encoding="utf-8", errors="replace")
    labeled_text, _ = loop_labels.label_c_source_text(source_text)
    lines = labeled_text.splitlines(keepends=True)
    skipping_printf = False
    for line_index, line in enumerate(lines):
        if skipping_printf:
            if ");" in line:
                skipping_printf = False
            filtered.append("// stripped by vitis_runner.py: host printf continuation\n")
            continue
        if re.match(r"\s*#\s*include\s+<stdio\.h>\s*", line):
            filtered.append("// stripped by vitis_runner.py: host stdio is not part of the synthesizable kernel\n")
            continue
        if re.match(r"\s*#\s*include\s+<math\.h>\s*", line) and "sqrtf" in source_text:
            filtered.append("// stripped by vitis_runner.py: APCC cannot compile this host math.h on Debian\n")
            filtered.append("extern float sqrtf(float);\n")
            continue
        if re.match(r"\s*#\s*pragma\s+HLS\s+(top|toplevel|resource)\b", line, re.IGNORECASE):
            filtered.append("// stripped by vitis_runner.py: file-scope HLS pragma handled by Tcl or unsupported by Vitis HLS\n")
            continue
        if re.match(r"\s*#\s*pragma\s+HLS\s+pipeline\b", line, re.IGNORECASE):
            filtered.append("// stripped by vitis_runner.py: labeled pipeline pragma emitted as Tcl directive\n")
            continue
        if re.search(r"\bprintf\s*\(", line):
            filtered.append("// stripped by vitis_runner.py: host printf tracing\n")
            if ");" not in line:
                skipping_printf = True
            continue
        filtered.append(line)
    staged_source.write_text("".join(filtered), encoding="utf-8")


def vitis_testbench(text: str) -> str:
    """Return a Vitis/APCC-compatible version of the shared C harness.

    Role:
    Remove ``stdio`` printing from the generated shared-input harness while
    preserving the same kernel call and argument values used by the other tools.

    Goal:
    Avoid APCC failures on host ``stdio.h`` definitions without skipping RTL
    co-simulation or changing benchmark inputs.
    """

    text = text.replace("#include <stdio.h>\n", "")
    text = re.sub(r'\n\s*fprintf\(stdout, "result=%d\\n", \(int\)result\);\n', "\n  (void)result;\n", text)
    return text


def csynth_xml_path(job: VitisJob) -> Path:
    """Return the expected Vitis HLS synthesis XML report path."""

    return job.project_dir / "solution1" / "syn" / "report" / f"{job.top_function}_csynth.xml"


def find_cosim_report(job: VitisJob) -> Path | None:
    """Find the Vitis HLS co-simulation report for a job if it exists."""

    report_dir = job.project_dir / "solution1" / "sim" / "report"
    candidates = [report_dir / f"{job.top_function}_cosim.rpt", report_dir / f"{job.top_function}_cosim.xml"]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    matches = sorted(report_dir.glob("*cosim*")) if report_dir.is_dir() else []
    return matches[0] if matches else None


def run_vivado_synthesis(job: VitisJob, args: argparse.Namespace) -> tuple[int | None, str | None]:
    """Run real Vivado synthesis on Vitis-generated RTL.

    Role:
    Read the RTL emitted by Vitis HLS, generate any floating-point IP described by
    Vitis ``*_ip.tcl`` files, synthesize the top module out-of-context for the
    requested FPGA part, and write standard utilization/timing reports.

    Goal:
    Populate normalized ``post_synth`` with real Vivado synthesis resource counts
    instead of displaying Vitis HLS estimates as area cost.
    """

    rtl_dir = vitis_rtl_dir(job)
    if not rtl_dir.is_dir():
        return 1, f"Vitis RTL directory not found: {rtl_dir}"
    job.vivado_dir.mkdir(parents=True, exist_ok=True)
    period = job.vivado_dir / f"period_{args.clock_period:g}.xdc"
    period.write_text(
        f"create_clock -name clk -period {args.clock_period:g} -waveform {{0.000 {args.clock_period / 2.0:g}}} [get_ports ap_clk]\n",
        encoding="utf-8",
    )
    write_vivado_tcl(job, args, rtl_dir, period)
    with job.vivado_log.open("w", encoding="utf-8") as log:
        try:
            proc = subprocess.run([args.vivado, "-mode", "tcl", "-source", str(job.vivado_script)], cwd=job.vivado_dir, stdout=log, stderr=subprocess.STDOUT, text=True, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            return None, f"Vivado post-HLS synthesis timed out after {args.timeout:g}s"
    if proc.returncode != 0:
        return proc.returncode, f"Vivado post-HLS synthesis failed with exit code {proc.returncode}"
    if not vivado_reports_exist(job):
        return 1, "Vivado post-HLS synthesis completed without reports"
    return 0, None


def write_vivado_tcl(job: VitisJob, args: argparse.Namespace, rtl_dir: Path, period: Path) -> None:
    """Write the Vivado Tcl script for post-HLS synthesis.

    Role:
    Encode all Vivado steps needed by the Vitis post-processing stage in a
    rerunnable script artifact.

    Goal:
    Keep real-area reporting transparent and debuggable for kernels that either
    use only plain Verilog or instantiate Vitis-generated Xilinx floating-point IP.
    """

    lines = [
        "set_param general.maxThreads 8",
        f"create_project -in_memory -part {args.part}",
    ]
    for ip_tcl in sorted(rtl_dir.glob("*_ip.tcl")):
        lines.append(f"source {{{ip_tcl}}}")
    lines.append(f"read_verilog [glob {{{rtl_dir}/*.v}}]")
    lines.extend(
        [
            "if {[llength [get_ips -quiet]] > 0} {",
            "  generate_target synthesis [get_ips]",
            f"  set ip_synth_verilog [glob -nocomplain {{{job.vivado_dir}/.gen/sources_1/ip/*/synth/*.v}}]",
            "  if {[llength $ip_synth_verilog] > 0} {",
            "    read_verilog $ip_synth_verilog",
            "  }",
            "}",
            f"read_xdc {{{period}}}",
            f"synth_design -top {job.top_function} -part {args.part} -no_iobuf -mode out_of_context",
            f"report_utilization > {{{vivado_util_report(job)}}}",
            f"report_timing > {{{vivado_timing_report(job)}}}",
            "exit",
        ]
    )
    job.vivado_script.write_text("\n".join(lines) + "\n", encoding="utf-8")


def vitis_rtl_dir(job: VitisJob) -> Path:
    """Return the Verilog RTL directory emitted by Vitis HLS."""

    return job.project_dir / "solution1" / "syn" / "verilog"


def vivado_util_report(job: VitisJob) -> Path:
    """Return the post-HLS Vivado utilization report path."""

    return job.vivado_dir / "post_synth_util.rpt"


def vivado_timing_report(job: VitisJob) -> Path:
    """Return the post-HLS Vivado timing report path."""

    return job.vivado_dir / "post_synth_timing.rpt"


def vivado_reports_exist(job: VitisJob) -> bool:
    """Return true when Vivado produced both required post-synthesis reports."""

    return vivado_util_report(job).is_file() and vivado_timing_report(job).is_file()


def parse_vitis_csynth(xml_path: Path) -> dict[str, Any]:
    """Parse Vitis HLS synthesis estimates from ``*_csynth.xml``.

    Role:
    Extract resource and timing estimates from the standard Vitis HLS XML report.

    Goal:
    Make Vitis HLS resource data visible while keeping it in the ``tool_estimate``
    block, distinct from downstream Vivado implementation metrics.
    """

    metrics = common_qor._metrics()
    if not xml_path.is_file():
        return metrics
    try:
        root = ET.parse(xml_path).getroot()
    except ET.ParseError:
        return metrics
    metrics.update(
        lut=_xml_int(root, {"LUT", "Luts"}),
        ff=_xml_int(root, {"FF", "Ff", "Register"}),
        dsp=_xml_int(root, {"DSP", "DSP48E", "DSP48E1"}),
        bram=_xml_int(root, {"BRAM_18K", "BRAM", "BRAM_18K_"}),
        tclk_ns=_xml_float(root, {"EstimatedClockPeriod", "AchievedClockPeriod"}),
    )
    target = _xml_float(root, {"TargetClockPeriod"})
    if metrics.get("tclk_ns") is not None and target is not None:
        metrics["slack_ns"] = target - metrics["tclk_ns"]
    return metrics


def parse_vitis_cosim_cycles(report_path: Path | None) -> int | None:
    """Parse an exact latency from a Vitis RTL co-simulation report."""

    if report_path is None or not report_path.is_file():
        return None
    text = report_path.read_text(encoding="utf-8", errors="replace")
    if report_path.suffix == ".xml":
        try:
            root = ET.fromstring(text)
        except ET.ParseError:
            root = None
        if root is not None:
            value = _xml_int(root, {"Average-caseLatency", "MaxLatency", "Latency"})
            if value is not None:
                return value
    for line in text.splitlines():
        if "Verilog" not in line and "VHDL" not in line:
            continue
        numbers = [int(item) for item in re.findall(r"\b\d+\b", line)]
        if numbers:
            return max(numbers)
    match = re.search(r"Latency\s*[:=]\s*(\d+)", text, re.IGNORECASE)
    return int(match.group(1)) if match else None


def normalize_result(result: VitisResult, *, part: str, clock_period_ns: float) -> dict[str, Any]:
    """Convert one Vitis native result to the common QoR schema."""

    csynth = Path(result.csynth_xml) if result.csynth_xml else Path("")
    cosim = Path(result.cosim_report) if result.cosim_report else None
    cycles = parse_vitis_cosim_cycles(cosim)
    estimates = parse_vitis_csynth(csynth)
    post_synth = common_qor.parse_vivado_report_files(
        Path(result.vivado_util_report) if result.vivado_util_report else Path(""),
        Path(result.vivado_timing_report) if result.vivado_timing_report else Path(""),
    )
    return {
        "benchmark": result.name,
        "tool": "vitis",
        "status": common_qor.normalize_status(result.status),
        "returncode": result.returncode,
        "elapsed_seconds": result.elapsed_seconds,
        "message": result.message,
        "target": {"part": part, "clock_period_ns": clock_period_ns},
        "sources": {"source": result.source, "top_function": result.top_function},
        "latency": {"cycles": cycles, "source": result.cosim_report if cycles is not None else None},
        "tool_estimate": estimates,
        "post_synth": post_synth,
        "artifacts": {
            "command": result.command,
            "log": result.log_path,
            "work_dir": result.project_dir,
            "script": result.script_path,
            "csynth_xml": result.csynth_xml,
            "cosim_report": result.cosim_report,
            "vivado_util_report": result.vivado_util_report,
            "vivado_timing_report": result.vivado_timing_report,
            "vivado_log": result.vivado_log_path,
        },
    }


def write_native_summary(path: Path, args: argparse.Namespace, results: list[VitisResult]) -> None:
    """Write the Vitis-native JSON summary."""

    payload = {
        "generated": datetime.now().isoformat(timespec="seconds"),
        "config": {
            "vitis_hls": args.vitis_hls,
            "vivado": args.vivado,
            "work_root": str(args.work_root),
            "part": args.part,
            "clock_period_ns": args.clock_period,
            "run_synthesis": args.run_synthesis,
            "run_simulation": args.run_simulation,
            "unsafe_math_optimizations": args.unsafe_math_optimizations,
            "timeout": args.timeout,
        },
        "results": [asdict(result) for result in results],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_normalized_summary(path: Path, args: argparse.Namespace, results: list[VitisResult]) -> None:
    """Write the common QoR JSON summary for Vitis results."""

    records = [normalize_result(result, part=args.part, clock_period_ns=args.clock_period) for result in results]
    payload = common_qor.build_report(
        tool="vitis",
        config={"vitis_hls": args.vitis_hls, "vivado": args.vivado, "part": args.part, "clock_period_ns": args.clock_period, "unsafe_math_optimizations": args.unsafe_math_optimizations},
        benchmarks=records,
    )
    common_qor.write_json(path, payload)


def print_table(results: list[VitisResult]) -> None:
    """Print a compact Vitis result table."""

    headers = ["benchmark", "status", "top", "cycles", "LUT", "FF", "BRAM", "DSP", "message"]
    rows: list[dict[str, str]] = []
    for result in results:
        estimate = parse_vitis_csynth(Path(result.csynth_xml) if result.csynth_xml else Path(""))
        rows.append(
            {
                "benchmark": result.name,
                "status": result.status,
                "top": result.top_function or "-",
                "cycles": _fmt(parse_vitis_cosim_cycles(Path(result.cosim_report) if result.cosim_report else None)),
                "LUT": _fmt(estimate.get("lut")),
                "FF": _fmt(estimate.get("ff")),
                "BRAM": _fmt(estimate.get("bram")),
                "DSP": _fmt(estimate.get("dsp")),
                "message": result.message or "",
            }
        )
    widths = [max(len(header), *(len(row[header]) for row in rows)) for header in headers]
    print(" | ".join(headers[index].ljust(widths[index]) for index in range(len(headers))))
    print("-+-".join("-" * width for width in widths))
    for row in rows:
        print(" | ".join(row[header].ljust(widths[index]) for index, header in enumerate(headers)))


def _selected_benchmarks(args: argparse.Namespace) -> set[str] | None:
    selected: list[str] = []
    if args.benchmarks:
        selected.extend(args.benchmarks)
    if args.bench:
        selected.extend(args.bench)
    return set(selected) if selected else None


def _validate_vitis_hls(path: str) -> str | None:
    if not Path(path).is_file():
        return f"vitis_hls executable not found: {path}"
    return None


def _validate_vivado(path: str) -> str | None:
    """Validate the Vivado executable used for real post-HLS synthesis."""

    if not Path(path).is_file():
        return f"Vivado executable not found: {path}"
    return None


def vitis_environment() -> dict[str, str]:
    """Return an environment suitable for Vitis HLS/APCC execution.

    Role:
    Add Debian's multiarch C runtime startup-object directory to ``LIBRARY_PATH``
    when the expected ``crt1.o`` and ``crti.o`` files exist.

    Goal:
    Let Vitis HLS RTL co-simulation link its generated executable on this local
    host without requiring users to export environment variables manually.
    """

    env = os.environ.copy()
    if (CRT_LIBRARY_PATH / "crt1.o").is_file() and (CRT_LIBRARY_PATH / "crti.o").is_file():
        existing = env.get("LIBRARY_PATH", "")
        entries = [str(CRT_LIBRARY_PATH)]
        if existing:
            entries.append(existing)
        env["LIBRARY_PATH"] = os.pathsep.join(entries)
    return env


def _skipped_result(name: str, message: str, job: VitisJob) -> VitisResult:
    return VitisResult(name, "SKIP", None, None, str(job.source), job.top_function, [], str(job.log), str(job.project_dir), str(job.script), message=message)


def _bare_skipped(name: str, message: str) -> VitisResult:
    return VitisResult(name, "SKIP", None, None, None, None, [], None, None, None, message=message)


def _xml_int(root: ET.Element, names: set[str]) -> int | None:
    value = _xml_text(root, names)
    if value is None:
        return None
    try:
        return int(float(value.strip()))
    except ValueError:
        return None


def _xml_float(root: ET.Element, names: set[str]) -> float | None:
    value = _xml_text(root, names)
    if value is None:
        return None
    try:
        return float(value.strip())
    except ValueError:
        return None


def _xml_text(root: ET.Element, names: set[str]) -> str | None:
    for elem in root.iter():
        tag = elem.tag.split("}", 1)[-1]
        if tag in names and elem.text:
            return elem.text
    return None


def _fmt(value: Any) -> str:
    return "-" if value is None else str(value)


if __name__ == "__main__":
    raise SystemExit(main())
