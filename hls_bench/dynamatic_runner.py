"""Batch-run local Dynamatic on repository benchmarks.

Role:
Discover C benchmarks, stage them into the filename layout expected by Dynamatic,
execute the installed Dynamatic script flow, and emit both native and normalized
JSON summaries.

Goal:
Make Dynamatic a first-class tool behind the repository's single CLI while
preserving failure visibility.  The runner does not strip pragmas, remove calls,
or otherwise simplify kernels to make them pass; unsupported source constructs
remain real tool failures in the report.

How:
Dynamatic v2.0.0 expects ``<source-dir>/<kernel-name>.c`` where
``kernel-name`` is the top-level C function.  Each benchmark is copied into an
isolated ``artifacts/tool-work/dynamatic/<benchmark>/src`` staging directory under that
name, with local headers and include directories copied alongside it.  The flow
runs ``compile.sh`` and ``write-hdl.sh`` by default; optional Vivado synthesis is
enabled only by ``--run-synthesis`` and uses this repository's local synthesis
wrapper so the Vivado part and clock are explicit benchmark-runner parameters.
"""

from __future__ import annotations

import argparse
import os
from dataclasses import asdict, dataclass
from datetime import datetime
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
from typing import Any

from hls_bench import config
from hls_bench import qor as common_qor
from hls_bench import harness as harness_gen
from hls_bench import loop_labels
from hls_bench import paths
from hls_bench.dynamatic import DynamaticFlowConfig, build_flow_commands, detect_script_interface


REPO_ROOT = Path(__file__).resolve().parent.parent
BENCHMARKS_DIR = paths.BENCHMARKS_ROOT
DEFAULT_WORK_ROOT = paths.DYNAMATIC_WORK_ROOT
DEFAULT_SUMMARY = DEFAULT_WORK_ROOT / "dynamatic_run_all.json"
DEFAULT_NORMALIZED_SUMMARY = DEFAULT_WORK_ROOT / "dynamatic_qor.json"
DEFAULT_CLOCK_PERIOD_NS = 10.0
DEFAULT_SYNTHESIS_PART = "xc7a100tcsg324-1"
LOCAL_SYNTHESIS_SCRIPT = REPO_ROOT / "hls_bench" / "scripts" / "dynamatic_synthesize.sh"
LOCAL_SIMULATION_SCRIPT = REPO_ROOT / "hls_bench" / "scripts" / "dynamatic_simulate.sh"
TRACE_DISABLE_PROLOGUE = (
    "#define TRACE 0\n"
    "#define OHLS_TRACE_H\n"
    "#define trace(VALUE) ((void)0)\n"
    "#define vcd_trace(VALUE) ((void)0)\n\n"
)
STDIO_DISABLE_BLOCK = (
    "\n"
    "/* Dynamatic cannot lower host-side debug I/O calls in kernel code.\n"
    " * These macros are applied only to staged sources so benchmark originals\n"
    " * remain unchanged while non-synthesizable tracing output is ignored. */\n"
    "#define printf(...) 0\n"
    "#define fprintf(...) 0\n"
    "#define puts(...) 0\n"
    "#define putchar(...) 0\n\n"
)


@dataclass(frozen=True)
class DynamaticJob:
    """One benchmark prepared for Dynamatic execution.

    Role:
    Carry immutable source, top-function, and staging paths from discovery into
    execution.

    Goal:
    Keep benchmark discovery deterministic and separate from subprocess running,
    so skipped and failed benchmarks can be reported uniformly.
    """

    name: str
    benchmark_dir: Path
    source: Path
    top_function: str
    staged_source_dir: Path
    simulation_source_dir: Path
    staged_source: Path
    output_dir: Path


@dataclass
class DynamaticResult:
    """Execution result for one Dynamatic benchmark.

    Role:
    Store per-stage return codes, logs, elapsed host time, generated command
    lines, and artifact locations.

    Goal:
    Preserve enough data to debug a failed compile/HDL/synthesis step without
    scraping terminal output.
    """

    name: str
    status: str
    returncode: int | None
    elapsed_seconds: float | None
    source: str | None
    top_function: str | None
    commands: dict[str, list[str]]
    stage_status: dict[str, str]
    logs: dict[str, str]
    output_dir: str | None
    message: str | None = None


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse Dynamatic runner options.

    Role:
    Define benchmark selection, installed Dynamatic location, staging paths,
    timeout policy, and optional synthesis behavior for the single CLI.

    Goal:
    Keep a safe default flow that compiles and writes HDL, while making full
    Vivado synthesis an explicit user choice.
    """

    parser = argparse.ArgumentParser(description="Run local Dynamatic over benchmark kernels.")
    parser.add_argument("--benchmarks", nargs="+", help="benchmark directory names to run")
    parser.add_argument("--bench", action="append", help="single benchmark name; may be repeated")
    parser.add_argument("--dynamatic-root", type=Path, default=_detect_default_dynamatic_root())
    parser.add_argument("--work-root", type=Path, default=DEFAULT_WORK_ROOT)
    parser.add_argument("--summary", type=Path, default=DEFAULT_SUMMARY)
    parser.add_argument("--normalized-summary", type=Path, default=DEFAULT_NORMALIZED_SUMMARY)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--clock-period", type=float, default=DEFAULT_CLOCK_PERIOD_NS)
    parser.add_argument("--vivado", default=_detect_default_vivado_command())
    parser.add_argument(
        "--part",
        default=DEFAULT_SYNTHESIS_PART,
        help="Vivado part passed to the repo-local Dynamatic synthesis wrapper",
    )
    parser.add_argument("--simple-buffers", action="store_true", help="legacy Dynamatic v2 simple-buffer mode")
    parser.add_argument(
        "--fp-units-generator",
        choices=["flopoco", "vivado"],
        default="flopoco",
        help="floating-point implementation requested from Dynamatic compile.sh",
    )
    parser.add_argument(
        "--simulator",
        choices=["verilator", "xsim", "ghdl", "vsim"],
        default="verilator",
        help="simulator passed to Dynamatic hls-verifier",
    )
    parser.add_argument("--run-simulation", action="store_true", help="run Dynamatic RTL simulation and record exact cycles")
    parser.add_argument("--run-synthesis", action="store_true", help="also run Dynamatic Vivado synthesis")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Run the Dynamatic batch command and return a process exit code.

    Role:
    Coordinate validation, benchmark discovery, staging, execution, and summary
    writing for Dynamatic.

    Goal:
    Provide the same batch-runner experience as LegUp and Bambu under the common
    CLI while accurately reporting unsupported or failed benchmarks.
    """

    args = parse_args(argv)
    results = run(args)
    failed = any(result.status in {"FAIL", "TIMEOUT", "SKIP"} for result in results)
    return 1 if failed else 0


def run(args: argparse.Namespace) -> list[DynamaticResult]:
    """Execute the configured Dynamatic benchmark batch.

    Role:
    Convert parsed CLI options into discovered jobs, run each job sequentially,
    and emit native plus normalized JSON artifacts.

    Goal:
    Keep execution order deterministic and logs easy to correlate with report
    entries while avoiding parallel writes inside Dynamatic output directories.
    """

    selected = _selected_benchmarks(args)
    jobs, results = discover_jobs(BENCHMARKS_DIR, args.work_root, selected)

    root_error = _validate_dynamatic_root(args.dynamatic_root)
    if root_error is not None:
        for job in jobs:
            results.append(_skipped_result(job.name, root_error, source=str(job.source), top_function=job.top_function))
        jobs = []

    if args.run_synthesis:
        vivado_error = _validate_vivado_command(args.vivado)
        if vivado_error is not None:
            for job in jobs:
                results.append(_skipped_result(job.name, vivado_error, source=str(job.source), top_function=job.top_function))
            jobs = []

    for job in jobs:
        results.append(run_job(job, args))

    _write_native_summary(args.summary, args, results)
    _write_normalized_summary(args.normalized_summary, args, results)
    _print_table(results)
    print(f"\nsummary: {args.summary}")
    print(f"normalized QoR: {args.normalized_summary}")
    return results


def discover_jobs(
    benchmarks_dir: Path,
    work_root: Path,
    selected: set[str] | None,
) -> tuple[list[DynamaticJob], list[DynamaticResult]]:
    """Discover and stage C benchmarks for Dynamatic.

    Role:
    Walk top-level benchmark directories, identify a single C kernel source and
    top-level function, and prepare an isolated source tree for each job.

    Goal:
    Report unsupported C++ or ambiguous benchmarks as skips instead of modifying
    their sources to fit Dynamatic.
    """

    jobs: list[DynamaticJob] = []
    skipped: list[DynamaticResult] = []
    for benchmark_dir in sorted(path for path in benchmarks_dir.iterdir() if path.is_dir()):
        if benchmark_dir.name in {"__pycache__", "benchmarks"}:
            continue
        if selected is not None and benchmark_dir.name not in selected:
            continue
        source_root = benchmark_dir / "src" if (benchmark_dir / "src").is_dir() else benchmark_dir
        source = _select_source(source_root)
        if source is None:
            skipped.append(_skipped_result(benchmark_dir.name, "could not identify a unique C kernel source"))
            continue
        if source.suffix != ".c" and benchmark_dir.name != "rv32i":
            skipped.append(_skipped_result(benchmark_dir.name, "Dynamatic runner currently supports C kernels only", source=str(source)))
            continue
        top_function = _infer_top_function(source, _read_makefile_variables(source_root / "Makefile"))
        if top_function is None:
            skipped.append(_skipped_result(benchmark_dir.name, "could not infer top function", source=str(source)))
            continue
        staged_source_dir = work_root / benchmark_dir.name / "src"
        simulation_source_dir = work_root / benchmark_dir.name / "sim_src"
        output_dir = work_root / benchmark_dir.name / "out"
        staged_source = staged_source_dir / f"{top_function}.c"
        try:
            _stage_source_tree(
                benchmark_dir,
                source,
                staged_source_dir,
                simulation_source_dir,
                staged_source,
                benchmark_dir.name,
                top_function,
            )
        except Exception as exc:
            skipped.append(_skipped_result(benchmark_dir.name, str(exc), source=str(source), top_function=top_function))
            continue
        jobs.append(
            DynamaticJob(
                name=benchmark_dir.name,
                benchmark_dir=benchmark_dir,
                source=source,
                top_function=top_function,
                staged_source_dir=staged_source_dir,
                simulation_source_dir=simulation_source_dir,
                staged_source=staged_source,
                output_dir=output_dir,
            )
        )

    if selected is not None:
        discovered = {job.name for job in jobs} | {result.name for result in skipped}
        for missing in sorted(selected - discovered):
            skipped.append(_skipped_result(missing, "benchmark directory not found"))
    return jobs, skipped


def run_job(job: DynamaticJob, args: argparse.Namespace) -> DynamaticResult:
    """Run one staged benchmark through Dynamatic.

    Role:
    Execute compile, HDL generation, and optional synthesis commands with a
    separate log for each stage.

    Goal:
    Stop at the first failed stage for the benchmark while preserving complete
    commands and logs for reproduction.
    """

    config = DynamaticFlowConfig(
        dynamatic_dir=args.dynamatic_root,
        source_dir=job.staged_source_dir,
        output_dir=job.output_dir,
        kernel_name=job.top_function,
        clock_period_ns=args.clock_period,
        synthesis_part=args.part,
        synthesis_script=LOCAL_SYNTHESIS_SCRIPT,
        simulation_script=LOCAL_SIMULATION_SCRIPT,
        use_simple_buffers=args.simple_buffers,
        fp_units_generator=args.fp_units_generator,
        use_vivado_fpu=args.fp_units_generator == "vivado",
        simulation_source_dir=job.simulation_source_dir,
        vivado_path=_vivado_install_root(args.vivado),
        simulator=args.simulator,
    )
    commands = build_flow_commands(config)
    logs = {
        "compile": str(_log_path(job, "compile")),
        "write_hdl": str(_log_path(job, "write_hdl")),
        "simulate": str(_log_path(job, "simulate")),
        "synthesize": str(_log_path(job, "synthesize")),
    }
    if args.dry_run:
        return DynamaticResult(
            name=job.name,
            status="DRY-RUN",
            returncode=None,
            elapsed_seconds=None,
            source=str(job.source),
            top_function=job.top_function,
            commands=commands,
            stage_status={
                "compile": "DRY-RUN",
                "write_hdl": "DRY-RUN",
                "simulate": "DRY-RUN" if args.run_simulation else "SKIP",
                "synthesize": "DRY-RUN" if args.run_synthesis else "SKIP",
            },
            logs=logs,
            output_dir=str(job.output_dir),
        )

    job.output_dir.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    stage_status: dict[str, str] = {}
    for stage in [
        "compile",
        "write_hdl",
        *( ["simulate"] if args.run_simulation else [] ),
        *( ["synthesize"] if args.run_synthesis else [] ),
    ]:
        env = _stage_env(stage, args.vivado, args.simulator)
        rc, message = _run_stage(stage, commands[stage], Path(logs[stage]), args.timeout, env=env)
        stage_status[stage] = "PASS" if rc == 0 else ("TIMEOUT" if rc is None else "FAIL")
        if rc != 0:
            return DynamaticResult(
                name=job.name,
                status=stage_status[stage],
                returncode=rc,
                elapsed_seconds=time.monotonic() - started,
                source=str(job.source),
                top_function=job.top_function,
                commands=commands,
                stage_status=stage_status,
                logs=logs,
                output_dir=str(job.output_dir),
                message=message or f"{stage} failed",
            )
    if not args.run_synthesis:
        stage_status["synthesize"] = "SKIP"
    if not args.run_simulation:
        stage_status["simulate"] = "SKIP"

    return DynamaticResult(
        name=job.name,
        status="PASS",
        returncode=0,
        elapsed_seconds=time.monotonic() - started,
        source=str(job.source),
        top_function=job.top_function,
        commands=commands,
        stage_status=stage_status,
        logs=logs,
        output_dir=str(job.output_dir),
    )


def _run_stage(
    stage: str,
    command: list[str],
    log_path: Path,
    timeout: float,
    env: dict[str, str] | None = None,
) -> tuple[int | None, str | None]:
    """Execute one Dynamatic stage with logging and timeout handling.

    Role:
    Run a subprocess exactly as generated by the adapter and capture combined
    stdout/stderr in a stage-specific log file.

    Goal:
    Make failures reproducible while avoiding partial terminal transcripts as the
    only diagnostic source.
    """

    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        log.write(f"# stage: {stage}\n")
        log.write(f"# started: {datetime.now().isoformat(timespec='seconds')}\n")
        log.write("# command: " + " ".join(command) + "\n\n")
        log.flush()
        try:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout, env=env)
            return completed.returncode, _last_error(log_path) if completed.returncode else None
        except subprocess.TimeoutExpired:
            log.write(f"\n# timed out after {timeout} seconds\n")
            return None, f"{stage} timed out after {timeout} seconds"


def _write_native_summary(path: Path, args: argparse.Namespace, results: list[DynamaticResult]) -> None:
    """Write the Dynamatic-native batch summary.

    Role:
    Serialize every job result with commands, logs, and stage status exactly as
    produced by the runner.

    Goal:
    Preserve debug-oriented execution detail separately from the normalized QoR
    report used for cross-tool comparison.
    """

    payload = {
        "generated": datetime.now().isoformat(timespec="seconds"),
        "config": {
            "dynamatic_root": str(args.dynamatic_root),
            "script_interface": detect_script_interface(args.dynamatic_root) if args.dynamatic_root.exists() else None,
            "work_root": str(args.work_root),
            "timeout_seconds": args.timeout,
            "clock_period_ns": args.clock_period,
            "vivado": args.vivado,
            "simple_buffers": args.simple_buffers,
            "fp_units_generator": args.fp_units_generator,
            "use_vivado_fpu": args.fp_units_generator == "vivado",
            "simulator": args.simulator,
            "run_simulation": args.run_simulation,
            "run_synthesis": args.run_synthesis,
            "dry_run": args.dry_run,
            "synthesis_part": args.part,
            "synthesis_script": str(LOCAL_SYNTHESIS_SCRIPT),
            "simulation_script": str(LOCAL_SIMULATION_SCRIPT),
        },
        "results": [asdict(result) for result in sorted(results, key=lambda item: item.name)],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _write_normalized_summary(path: Path, args: argparse.Namespace, results: list[DynamaticResult]) -> None:
    """Write the common cross-tool QoR report for Dynamatic.

    Role:
    Convert Dynamatic stage results and optional Vivado synthesis reports into
    the same schema emitted by LegUp and Bambu.

    Goal:
    Make Dynamatic visible in aggregate comparisons while leaving unavailable
    pre-synthesis area and latency fields explicitly null.
    """

    records = [_normalize_result(result, args) for result in sorted(results, key=lambda item: item.name)]
    payload = common_qor.build_report(
        tool="dynamatic",
        config={
            "dynamatic_root": str(args.dynamatic_root),
            "script_interface": detect_script_interface(args.dynamatic_root) if args.dynamatic_root.exists() else None,
            "work_root": str(args.work_root),
            "summary": str(args.summary),
            "normalized_summary": str(path),
            "timeout_seconds": args.timeout,
            "clock_period_ns": args.clock_period,
            "vivado": args.vivado,
            "simple_buffers": args.simple_buffers,
            "fp_units_generator": args.fp_units_generator,
            "use_vivado_fpu": args.fp_units_generator == "vivado",
            "simulator": args.simulator,
            "run_simulation": args.run_simulation,
            "run_synthesis": args.run_synthesis,
            "dry_run": args.dry_run,
            "synthesis_part": args.part,
            "synthesis_script": str(LOCAL_SYNTHESIS_SCRIPT),
            "simulation_script": str(LOCAL_SIMULATION_SCRIPT),
        },
        benchmarks=records,
    )
    common_qor.write_json(path, payload)


def _normalize_result(result: DynamaticResult, args: argparse.Namespace) -> dict[str, Any]:
    """Convert one Dynamatic result to the common QoR record shape.

    Role:
    Map stage status, artifacts, and optional post-synthesis reports to the
    shared schema.

    Goal:
    Keep missing estimates and latency explicit while exposing Vivado metrics
    whenever synthesis was requested and completed.
    """

    output_dir = Path(result.output_dir) if result.output_dir else None
    synth_dir = output_dir / "synth" if output_dir else None
    post_synth = common_qor.parse_vivado_report_files(
        synth_dir / "utilization_post_syn.rpt", synth_dir / "timing_post_syn.rpt"
    ) if synth_dir and result.stage_status.get("synthesize") == "PASS" else common_qor.parse_vivado_report_files(Path(""), Path(""))
    return {
        "benchmark": result.name,
        "tool": "dynamatic",
        "status": common_qor.normalize_status(result.status),
        "returncode": result.returncode,
        "elapsed_seconds": result.elapsed_seconds,
        "message": result.message,
        "target": {
            "part": args.part if args.run_synthesis else None,
            "clock_period_ns": args.clock_period,
        },
        "sources": {
            "source": result.source,
            "top_function": result.top_function,
        },
        "latency": _parse_dynamatic_simulation_cycles(Path(result.output_dir) / "sim" / "report.txt")
        if result.output_dir and result.stage_status.get("simulate") == "PASS"
        else {"cycles": None, "source": None},
        "tool_estimate": common_qor.parse_vivado_report_files(Path(""), Path("")),
        "post_synth": post_synth,
        "artifacts": {
            "command": result.commands,
            "log": result.logs,
            "work_dir": result.output_dir,
        },
    }


def _print_table(results: list[DynamaticResult]) -> None:
    """Print a compact Dynamatic status table.

    Role:
    Present benchmark, source, top function, stage states, and messages in a
    human-readable form after JSON reports are written.

    Goal:
    Make quick interactive runs easy to inspect without replacing the detailed
    log files.
    """

    headers = ["benchmark", "status", "top", "compile", "hdl", "sim", "synth", "message"]
    rows = []
    for result in sorted(results, key=lambda item: item.name):
        rows.append([
            result.name,
            result.status,
            result.top_function or "-",
            result.stage_status.get("compile", "-"),
            result.stage_status.get("write_hdl", "-"),
            result.stage_status.get("simulate", "-"),
            result.stage_status.get("synthesize", "-"),
            result.message or "",
        ])
    widths = [max(len(row[index]) for row in [headers, *rows]) for index in range(len(headers))]
    print(" | ".join(headers[index].ljust(widths[index]) for index in range(len(headers))))
    print("-+-".join("-" * width for width in widths))
    for row in rows:
        print(" | ".join(row[index].ljust(widths[index]) for index in range(len(headers))))


def _stage_source_tree(
    benchmark_dir: Path,
    source: Path,
    staged_source_dir: Path,
    simulation_source_dir: Path,
    staged_source: Path,
    benchmark_name: str,
    top_function: str,
) -> None:
    """Stage a benchmark source tree and generate a Dynamatic harness.

    Role:
    Copy the kernel source into the exact ``<top_function>.c`` filename expected
    by Dynamatic, copy local headers beside it, and generate a separate
    ``main_dynamatic.c`` harness for later simulation-oriented use.

    Goal:
    Keep the original benchmark sources untouched while matching Dynamatic's
    compile contract: ``compile.sh`` receives the kernel translation unit only,
    not a combined kernel-plus-main source that can introduce unsupported host
    harness calls into the frontend.
    """

    if staged_source_dir.exists():
        shutil.rmtree(staged_source_dir)
    staged_source_dir.mkdir(parents=True, exist_ok=True)
    if simulation_source_dir.exists():
        shutil.rmtree(simulation_source_dir)
    simulation_source_dir.mkdir(parents=True, exist_ok=True)
    kernel_text = source.read_text(encoding="utf-8", errors="replace")
    staged_kernel_text = _prepare_dynamatic_kernel_source(
        _translate_kernel_source_for_dynamatic_c(kernel_text, benchmark_name, rewrite_rv32i_byte_load=True),
        disable_stdio_calls=True,
    )
    staged_source.write_text(loop_labels.label_c_source_text(staged_kernel_text)[0], encoding="utf-8")
    source_root = benchmark_dir / "src" if (benchmark_dir / "src").is_dir() else benchmark_dir
    for header in sorted(source_root.glob("*.h")):
        header_text = _translate_kernel_source_for_dynamatic_c(
            header.read_text(encoding="utf-8", errors="replace"),
            benchmark_name,
        )
        (staged_source_dir / header.name).write_text(header_text, encoding="utf-8")
        (simulation_source_dir / header.name).write_text(header_text, encoding="utf-8")
    for header in sorted(source_root.glob("*.hpp")):
        shutil.copy2(header, staged_source_dir / header.name)
        shutil.copy2(header, simulation_source_dir / header.name)
    include_dir = source_root / "include"
    if include_dir.is_dir():
        shutil.copytree(include_dir, staged_source_dir / "include", dirs_exist_ok=True)
        shutil.copytree(include_dir, simulation_source_dir / "include", dirs_exist_ok=True)

    signature = harness_gen.find_top_signature(source.read_text(encoding="utf-8", errors="replace"))
    if signature is None:
        raise ValueError(f"could not infer a top signature for {benchmark_name}")

    header = harness_gen.select_integration_header(source_root, top_function)
    if benchmark_name == "rv32i":
        harness = _generate_rv32i_dynamatic_harness()
    else:
        harness = harness_gen.generate_dynamatic_harness(
            signature,
            header.name if header else None,
        )
    harness_path = staged_source_dir / "main_dynamatic.c"
    harness_path.write_text(
        harness,
        encoding="utf-8",
    )
    simulation_kernel_text = _prepare_dynamatic_kernel_source(
        _translate_kernel_source_for_dynamatic_c(kernel_text, benchmark_name, rewrite_rv32i_byte_load=True),
        disable_stdio_calls=False,
    )
    (simulation_source_dir / f"{top_function}.c").write_text(
        loop_labels.label_c_source_text(simulation_kernel_text)[0]
        + "\n"
        + _rv32i_gcd_image_for_combined_simulation(benchmark_name, source_root)
        + "\n"
        + harness,
        encoding="utf-8",
    )


def _translate_kernel_source_for_dynamatic_c(
    source_text: str,
    benchmark_name: str,
    *,
    rewrite_rv32i_byte_load: bool = False,
) -> str:
    """Translate benchmark-local C++-ish kernels into C for Dynamatic staging.

    Role:
    Apply only source-staging edits needed by Dynamatic's C-only frontend while
    leaving canonical benchmark files unchanged for the other HLS tools.

    Goal:
    Let ``rv32i_kernel.cpp`` enter the Dynamatic C flow as ``cpu_run.c`` without
    changing the computation, test inputs, or tool-visible top-level signature.
    """

    if benchmark_name != "rv32i":
        return source_text
    source_text = re.sub(
        r"0[bB]([01]+)",
        lambda match: hex(int(match.group(1), 2)),
        source_text,
    )
    # Dynamatic infers top-level memrefs from literal C array bounds.  Preserve
    # RV32I's 16 MiB architectural space while exposing its exact 4 Miword
    # bounds to the staging-only source parser.
    source_text = source_text.replace("MEM_SIZE / 4", "4194304")
    if not rewrite_rv32i_byte_load:
        return source_text
    pointer_select_start = "value = (addr & 3u) == 0 ? mem0[index]"
    pointer_select_end = "mem3[index];"
    scalar_switch = """switch (addr & 3u) {
                case 0: value = (uint32_t)mem0[index]; break;
                case 1: value = (uint32_t)mem1[index]; break;
                case 2: value = (uint32_t)mem2[index]; break;
                default: value = (uint32_t)mem3[index]; break;
                }"""
    if source_text.count(pointer_select_start) != 1:
        raise ValueError("RV32I Dynamatic staging expected exactly one byte-lane pointer select")
    # Dynamatic cannot translate pointer-valued select/phi nodes.  Select the
    # loaded scalar instead, preserving the exact byte-lane memory semantics.
    start = source_text.index(pointer_select_start)
    end = source_text.index(pointer_select_end, start) + len(pointer_select_end)
    return source_text[:start] + scalar_switch + source_text[end:]


def _generate_rv32i_dynamatic_harness() -> str:
    """Generate a Dynamatic verifier harness for the RV32I GCD program.

    Role:
    Initialize the same instruction/data byte-lane memories as ``main_rv32i.cpp``
    and call ``cpu_run`` through Dynamatic's ``CALL_KERNEL`` macro.

    Goal:
    Preserve the deterministic embedded GCD workload for exact Dynamatic RTL
    cycle counting instead of using arbitrary pseudo-random array contents.
    """

    return """#include <stdint.h>
#include <string.h>
#include "rv32i.h"
#include "gcd_image.h"
#include "dynamatic/Integration.h"

static uint32_t instr_mem[MEM_SIZE / 4];
static uint8_t mem0[MEM_SIZE / 4];
static uint8_t mem1[MEM_SIZE / 4];
static uint8_t mem2[MEM_SIZE / 4];
static uint8_t mem3[MEM_SIZE / 4];

static void clear_memories(void) {
  memset(instr_mem, 0, sizeof(instr_mem));
  memset(mem0, 0, sizeof(mem0));
  memset(mem1, 0, sizeof(mem1));
  memset(mem2, 0, sizeof(mem2));
  memset(mem3, 0, sizeof(mem3));
}

static void load_program_image(const unsigned char *image, unsigned int image_len) {
  clear_memories();
  for (uint32_t i = 0; i < image_len && i < MEM_SIZE; ++i) {
    uint32_t base_addr = i / 4u;
    uint32_t instr_addr = base_addr * 4u;
    uint8_t byte_value = image[i];
    switch (i & 3u) {
    case 0:
      instr_mem[instr_addr] = (instr_mem[instr_addr] & 0xFFFFFF00u) | (uint32_t)byte_value;
      mem0[base_addr] = byte_value;
      break;
    case 1:
      instr_mem[instr_addr] = (instr_mem[instr_addr] & 0xFFFF00FFu) | ((uint32_t)byte_value << 8);
      mem1[base_addr] = byte_value;
      break;
    case 2:
      instr_mem[instr_addr] = (instr_mem[instr_addr] & 0xFF00FFFFu) | ((uint32_t)byte_value << 16);
      mem2[base_addr] = byte_value;
      break;
    default:
      instr_mem[instr_addr] = (instr_mem[instr_addr] & 0x00FFFFFFu) | ((uint32_t)byte_value << 24);
      mem3[base_addr] = byte_value;
      break;
    }
  }
}

static uint32_t read_memory_word(uint32_t byte_addr) {
  uint32_t base_addr = byte_addr >> 2;
  return ((uint32_t)mem0[base_addr]) |
         (((uint32_t)mem1[base_addr]) << 8) |
         (((uint32_t)mem2[base_addr]) << 16) |
         (((uint32_t)mem3[base_addr]) << 24);
}

int main(void) {
  uint32_t init_pc = 0u;
  load_program_image(gcd_bin, gcd_bin_len);
  CALL_KERNEL(cpu_run, init_pc, instr_mem, mem0, mem1, mem2, mem3);
  return read_memory_word(520u) == 3u && read_memory_word(524u) == 1u ? 0 : 1;
}
"""


def _rv32i_gcd_image_for_combined_simulation(benchmark_name: str, source_root: Path) -> str:
    """Return embedded GCD image definitions for Dynamatic's combined sim source.

    Role:
    Inline ``gcd_image.c`` into the generated simulation translation unit for the
    special RV32I harness.

    Goal:
    Ensure the C reference executable used by Dynamatic's verifier defines
    ``gcd_bin`` and ``gcd_bin_len`` without relying on separate compilation of
    benchmark support files.
    """

    if benchmark_name != "rv32i":
        return ""
    gcd_image = source_root / "gcd_image.c"
    if not gcd_image.is_file():
        raise FileNotFoundError(f"missing rv32i GCD image source: {gcd_image}")
    return gcd_image.read_text(encoding="utf-8", errors="replace")


def _prepare_dynamatic_kernel_source(source_text: str, *, disable_stdio_calls: bool) -> str:
    """Return the kernel source text used by Dynamatic compilation.

    Role:
    Apply staging-local compatibility edits for constructs that are not part of
    the kernel computation but are known to break Dynamatic's LLVM frontend:
    Clang annotation metadata, and optionally debug/trace I/O calls.

    Goal:
    Preserve the benchmark's computational source while avoiding known frontend
    crashes on host-side I/O such as ``printf`` and on ``llvm.global.annotations``
    globals emitted by ``__attribute__((annotate("hls.toplevel")))``.  Stdio call
    macros are used only for the compile source because Dynamatic's verifier
    harness is compiled as C++ and includes headers such as ``<cstdio>`` that must
    see the real declarations.
    """

    source_text = _strip_dynamatic_unsupported_annotations(source_text)
    lines = source_text.splitlines(keepends=True)
    insert_at = 0
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("#include"):
            insert_at = index + 1
    # RV32I separates local and system headers with comments.  Scan the whole
    # preamble so stdio replacement macros cannot rewrite declarations in a
    # later system header.
    if disable_stdio_calls:
        lines.insert(insert_at, STDIO_DISABLE_BLOCK)
    return TRACE_DISABLE_PROLOGUE + "".join(lines)


def _strip_dynamatic_unsupported_annotations(source_text: str) -> str:
    """Remove HLS top annotations from the Dynamatic-only staged C source.

    Role:
    Delete Clang ``annotate("hls.toplevel")`` attributes before Dynamatic's Clang
    invocation can lower them into ``llvm.global.annotations`` globals.

    Goal:
    Avoid Dynamatic's ``translate-llvm-to-std`` crash in ``translateGlobalVars``
    while keeping benchmark originals unchanged and preserving the top-function
    choice through the runner's explicit command-line ``kernel_name`` argument.
    """

    source_text = re.sub(
        r'__attribute__\s*\(\(\s*annotate\("hls\.toplevel"\)\s*\)\)\s*',
        "",
        source_text,
    )
    source_text = re.sub(
        r'\[\[\s*clang::annotate\("hls\.toplevel"\)\s*\]\]\s*',
        "",
        source_text,
    )
    return source_text


def _selected_benchmarks(args: argparse.Namespace) -> set[str] | None:
    """Collect benchmark selection options into one set.

    Role:
    Merge repeated ``--bench`` values with the list-style ``--benchmarks`` option.

    Goal:
    Let the single CLI support both LegUp-style and Bambu-style selection names.
    """

    selected = set(args.benchmarks or []) | set(args.bench or [])
    return selected or None


def _validate_dynamatic_root(root: Path) -> str | None:
    """Validate that a Dynamatic root contains the scripts used by the runner.

    Role:
    Check the installed ``~/.local`` source tree before reporting per-benchmark
    skips.

    Goal:
    Fail clearly when the Dynamatic installation cannot be found instead of
    producing subprocess file-not-found exceptions for every benchmark.
    """

    scripts = root / "tools" / "dynamatic" / "scripts"
    required = [
        scripts / "compile.sh",
        scripts / "write-hdl.sh",
        scripts / "utils.sh",
        root / "bin" / "clang",
        root / "bin" / "opt",
        root / "bin" / "source-rewriter",
        root / "bin" / "dynamatic-opt",
        root / "bin" / "exp-frequency-profiler",
        root / "bin" / "export-dot",
        root / "bin" / "export-cfg",
        root / "bin" / "export-rtl",
        root / "build" / "bin" / "translate-llvm-to-std",
        root / "build" / "lib" / "DynPragmasPlugin.so",
        root / "build" / "lib" / "MemDepAnalysis.so",
        root / "build" / "python3-venv" / "bin" / "activate",
        root / "build" / "include" / "clang_headers",
        LOCAL_SYNTHESIS_SCRIPT,
        LOCAL_SIMULATION_SCRIPT,
    ]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        return "missing Dynamatic build artifacts: " + ", ".join(missing)
    return None


def _detect_default_dynamatic_root() -> Path:
    """Choose the most plausible local Dynamatic checkout.

    Role:
    Prefer a usable local install when one exists, but still return a concrete
    path when only an incomplete checkout is present.

    Goal:
    Keep the runner usable on machines where Dynamatic is installed outside the
    original ``~/.local/src`` layout, while still letting validation report a
    precise missing-artifact error when the build is incomplete.
    """

    env_root = os.environ.get("DYNAMATIC_ROOT")
    candidates = [
        Path(env_root).expanduser() if env_root else None,
        REPO_ROOT.parent / "dynamatic",
        Path.home() / "dynamatic",
        Path.home() / ".local" / "src" / "dynamatic-v2.0.0",
    ]
    concrete = [candidate for candidate in candidates if candidate is not None]
    for candidate in concrete:
        if _validate_dynamatic_root(candidate) is None:
            return candidate
    for candidate in concrete:
        if (candidate / "tools" / "dynamatic" / "scripts" / "compile.sh").exists():
            return candidate
    return concrete[0] if concrete else Path.home() / ".local" / "src" / "dynamatic-v2.0.0"


def _detect_default_vivado_command() -> str:
    """Resolve a usable Vivado command for the local synthesis wrapper."""

    path_vivado = shutil.which(config.VIVADO)
    if path_vivado:
        return path_vivado
    configured = Path(config.VIVADO)
    if configured.is_file():
        return str(configured)
    return config.VIVADO


def _validate_vivado_command(vivado_cmd: str) -> str | None:
    """Validate that Vivado is callable before synthesis starts."""

    if shutil.which(vivado_cmd):
        return None
    vivado_path = Path(vivado_cmd)
    if vivado_path.is_file() and os.access(vivado_path, os.X_OK):
        return None
    return f"Vivado command not found or not executable: {vivado_cmd}"


def _vivado_env(vivado_cmd: str) -> dict[str, str] | None:
    """Expose an absolute Vivado path to the synthesis wrapper via PATH."""

    vivado_path = Path(vivado_cmd)
    if vivado_path.is_file():
        env = os.environ.copy()
        env["PATH"] = str(vivado_path.parent) + os.pathsep + env.get("PATH", "")
        return env
    return None


def _stage_env(stage: str, vivado_cmd: str, simulator: str = "verilator") -> dict[str, str] | None:
    """Return environment overrides needed by tool subprocess stages.

    Role:
    Keep stage-specific executable lookup adjustments out of the main execution
    loop.

    Goal:
    Make synthesis find the selected Vivado binary and make simulation prefer the
    coherent oss-cad-suite Verilator installation when it is available locally.
    """

    if stage == "synthesize":
        return _vivado_env(vivado_cmd)
    if stage == "simulate":
        env = os.environ.copy()
        vivado_path = Path(vivado_cmd)
        if vivado_path.is_file():
            env["PATH"] = str(vivado_path.parent) + os.pathsep + env.get("PATH", "")
        oss_verilator = Path.home() / "oss-cad-suite" / "bin"
        if simulator == "verilator" and oss_verilator.is_dir():
            env["PATH"] = str(oss_verilator) + os.pathsep + env.get("PATH", "")
        return env
    return None


def _vivado_install_root(vivado_cmd: str) -> Path:
    """Return the Vivado installation root expected by Dynamatic simulation.

    Role:
    Convert a command such as ``/opt/.../Vivado/2023.2/bin/vivado`` into the
    installation directory consumed by Dynamatic's ``simulate.sh``.

    Goal:
    Keep synthesis using the executable path while simulation receives the
    broader Vivado directory needed for optional support files.
    """

    vivado_path = Path(vivado_cmd)
    if vivado_path.name == "vivado" and vivado_path.parent.name == "bin":
        return vivado_path.parent.parent
    return vivado_path


def _parse_dynamatic_simulation_cycles(report_path: Path) -> dict[str, int | str | None]:
    """Read the exact cycle count reported by Dynamatic's RTL simulator.

    Role:
    Parse the ``hls-verifier`` report emitted by ``simulate.sh`` after HDL
    simulation completes successfully.

    Goal:
    Populate the common latency field only from an execution-backed simulator
    result, never from schedule estimates or synthesis timing reports.
    """

    if not report_path.is_file():
        return {"cycles": None, "source": None}
    text = report_path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"Simulation done!\s+Latency\s*=\s*(\d+)\s+cycles", text)
    if not match:
        return {"cycles": None, "source": None}
    return {"cycles": int(match.group(1)), "source": str(report_path)}


def _read_makefile_variables(makefile: Path) -> dict[str, str]:
    """Read simple Makefile assignments used for benchmark metadata.

    Role:
    Extract ``KERNEL`` and ``TOP_MODULE`` style declarations without invoking
    Make.

    Goal:
    Prefer explicit benchmark metadata over source-code heuristics when staging
    Dynamatic jobs.
    """

    variables: dict[str, str] = {}
    if not makefile.exists():
        return variables
    assignment = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(?::=|=)\s*(.*)$")
    for line in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
        match = assignment.match(line.strip())
        if match:
            variables[match.group(1)] = match.group(2).split("#", 1)[0].strip()
    return variables


def _select_source(benchmark_dir: Path) -> Path | None:
    """Select one likely C kernel source from a benchmark directory.

    Role:
    Resolve Makefile ``KERNEL`` metadata first, then fall back to conservative
    source-file ranking that avoids testbenches and tracing stubs.

    Goal:
    Avoid accidentally compiling host testbench code as a Dynamatic kernel.
    """

    variables = _read_makefile_variables(benchmark_dir / "Makefile")
    kernel = variables.get("KERNEL")
    if kernel:
        for token in kernel.split():
            candidate = benchmark_dir / token
            if candidate.exists() and candidate.suffix in {".c", ".cpp", ".cc", ".cxx"}:
                return candidate
    all_sources = sorted(
        path for suffix in ("*.c", "*.cc", "*.cpp", "*.cxx") for path in benchmark_dir.glob(suffix)
    )
    excluded = ("main", "trace_stub", "trace_stubs", "_ref", "ref_trace", "ohls_stream_sim")
    kernels = [path for path in all_sources if "kernel" in path.stem]
    candidates = kernels or [path for path in all_sources if not any(fragment in path.stem for fragment in excluded)]
    return candidates[0] if len(candidates) == 1 else None


def _infer_top_function(source: Path, variables: dict[str, str]) -> str | None:
    """Infer the C top function expected by Dynamatic.

    Role:
    Use Makefile ``TOP_MODULE`` metadata when present, otherwise locate the first
    non-static C function definition in the selected kernel source.

    Goal:
    Pass a kernel name that matches the actual entry point and Dynamatic's staged
    filename convention.
    """

    if variables.get("TOP_MODULE"):
        return variables["TOP_MODULE"].split()[0]
    text = source.read_text(encoding="utf-8", errors="replace")
    function_definition = re.compile(
        r"(?:^|\n)\s*(?!static\b)(?:[A-Za-z_][A-Za-z0-9_]*\s+[\*&\s]*)+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{",
        re.MULTILINE,
    )
    for match in function_definition.finditer(text):
        name = match.group(1)
        if name != "main":
            return name
    return None


def _skipped_result(name: str, message: str, source: str | None = None, top_function: str | None = None) -> DynamaticResult:
    """Create a skipped result with a clear reason.

    Role:
    Represent discovery, validation, or support-limit skips using the same result
    object as executed jobs.

    Goal:
    Keep summaries complete and make unsupported benchmarks visible rather than
    silently dropping them from reports.
    """

    return DynamaticResult(
        name=name,
        status="SKIP",
        returncode=None,
        elapsed_seconds=None,
        source=source,
        top_function=top_function,
        commands={},
        stage_status={},
        logs={},
        output_dir=None,
        message=message,
    )


def _log_path(job: DynamaticJob, stage: str) -> Path:
    """Return the stage log path for one job.

    Role:
    Centralize log naming under ``artifacts/tool-work/dynamatic/<bench>/logs``.

    Goal:
    Make generated log paths stable across native and normalized reports.
    """

    return job.output_dir.parent / "logs" / f"{stage}.log"


def _last_error(log_path: Path) -> str | None:
    """Extract a concise error line from a stage log.

    Role:
    Find the last line that looks like a failure, falling back to the final log
    line when no explicit error marker is present.

    Goal:
    Put actionable failure context in JSON summaries without truncating the full
    log artifact.
    """

    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines() if log_path.exists() else []
    interesting = [line for line in lines if re.search(r"\b(error|ERROR|failed|FAILURE)\b", line)]
    return interesting[-1] if interesting else (lines[-1] if lines else None)


if __name__ == "__main__":
    raise SystemExit(main())
