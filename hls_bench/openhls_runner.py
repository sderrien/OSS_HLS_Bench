"""Run OpenHLS/VibeHLS benchmark targets and normalize QoR reports.

Role:
Stage kernels from the configured benchmark root into an isolated OpenHLS work tree,
generate the ``vhls_project.json`` manifest expected by ``vhls-cli``, run exact
RTL verification and Vivado synthesis targets, and convert their artifacts into
the shared QoR JSON schema used by the other HLS runners.

Goal:
Make OpenHLS comparable with LegUp, PANDA/Bambu, Dynamatic, and Vitis while
preserving the cross-tool rules used in this repository: cycle counts come from
tool/runtime execution artifacts, and area/timing comes from real post-synthesis
Vivado reports rather than schedule estimates.

How:
The runner borrows the target selection and artifact conventions from
``~/VibeHLS/test/benchmarks/run_all.py`` but avoids running in-place.  Each job is
staged under ``artifacts/tool-work/openhls/<benchmark>`` so generated
``output`` trees and logs do not modify the canonical kernel sources.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from typing import Any

from hls_bench import inputs as common_inputs
from hls_bench import config
from hls_bench import loop_labels
from hls_bench import paths
from hls_bench import qor as common_qor


REPO_ROOT = paths.REPO_ROOT
KERNELS_DIR = paths.BENCHMARKS_ROOT
DEFAULT_WORK_ROOT = paths.OPENHLS_WORK_ROOT
DEFAULT_SUMMARY = DEFAULT_WORK_ROOT / "openhls_run_all.json"
DEFAULT_NORMALIZED_SUMMARY = DEFAULT_WORK_ROOT / "openhls_qor.json"
DEFAULT_OPENHLS_ROOT = config.OPENHLS_ROOT
DEFAULT_VHLS_CLI = config.VHLS_CLI
DEFAULT_VIVADO = config.VIVADO
DEFAULT_LLVM_BIN = str(config.OPENHLS_LLVM_BIN)
DEFAULT_CIRCT_BIN = str(config.OPENHLS_CIRCT_BIN)
DEFAULT_PART = "xc7a100tcsg324-1"


@dataclass(frozen=True)
class OpenHLSJob:
    """One staged OpenHLS benchmark invocation.

    Role:
    Carry the source benchmark directory, staged work directory, generated
    manifest path, and key metadata needed to run ``vhls-cli`` targets.

    Goal:
    Keep target execution and report parsing independent from source discovery.
    """

    name: str
    source_dir: Path
    work_dir: Path
    manifest_path: Path
    project_name: str
    top_function: str
    output_base: str
    dse: dict[str, Any] | None = None


@dataclass
class OpenHLSResult:
    """Native execution result for one OpenHLS benchmark.

    Role:
    Preserve target return codes, logs, parsed artifacts, and failure messages in
    a script-friendly form before conversion to normalized QoR.

    Goal:
    Make failed target stages visible without substituting estimates for missing
    cycles or synthesis reports.
    """

    name: str
    status: str
    returncode: int | None
    elapsed_seconds: float | None
    top_function: str | None
    work_dir: str | None
    manifest: str | None
    target_status: dict[str, str]
    logs: dict[str, str]
    latency_cycles: int | None = None
    latency_source: str | None = None
    vivado_summary: str | None = None
    invalid_artifacts: list[str] | None = None
    message: str | None = None
    dse: dict[str, Any] | None = None


def main(argv: list[str] | None = None) -> int:
    """Run OpenHLS benchmarks from command-line arguments.

    Role:
    Parse runner options, stage selected kernels, execute the requested OpenHLS
    targets, write native and normalized JSON summaries, and print a compact
    table for interactive use.

    Goal:
    Provide a direct ``python -m hls_bench.cli openhls`` entry point that behaves like the
    existing tool runners and can be called by the parallel scheduler.
    """

    args = parse_args(argv)
    selected = selected_benchmarks(args)
    jobs, skipped = discover_jobs(args.work_root, selected, args)
    results: list[OpenHLSResult] = [*skipped]
    for job in jobs:
        results.append(run_job(job, args))
    write_native_summary(args.summary, args, results)
    write_normalized_summary(args.normalized_summary, args, results)
    print_summary(results)
    print(f"\nsummary: {args.summary}")
    print(f"normalized QoR: {args.normalized_summary}")
    return 0 if all(result.status in {"PASS", "SKIP", "DRY-RUN"} for result in results) else 1


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    """Parse OpenHLS runner options."""

    parser = argparse.ArgumentParser(description="Run OpenHLS/VibeHLS benchmark targets.", allow_abbrev=False)
    parser.add_argument("--benchmarks", nargs="+", help="benchmark directory names to run")
    parser.add_argument("--bench", action="append", help="single benchmark name; may be repeated")
    parser.add_argument("--vhls-cli", default=DEFAULT_VHLS_CLI, help="vhls-cli executable")
    parser.add_argument("--vivado", default=DEFAULT_VIVADO, help="Vivado executable used by vivado_synth")
    parser.add_argument("--llvm-bin", default=os.environ.get("VIBEHLS_LLVM_BIN", DEFAULT_LLVM_BIN), help="LLVM bin directory used by VibeHLS")
    parser.add_argument("--circt-bin", default=os.environ.get("VIBEHLS_CIRCT_BIN", DEFAULT_CIRCT_BIN), help="CIRCT bin directory used by VibeHLS")
    parser.add_argument("--work-root", type=Path, default=DEFAULT_WORK_ROOT)
    parser.add_argument("--summary", type=Path, default=DEFAULT_SUMMARY)
    parser.add_argument("--normalized-summary", type=Path, default=DEFAULT_NORMALIZED_SUMMARY)
    parser.add_argument("--timeout", type=float, default=600.0, help="per-target timeout in seconds")
    parser.add_argument("--clock-period", type=float, default=10.0, help="target clock period in ns")
    parser.add_argument("--part", default=DEFAULT_PART, help="OpenHLS/Vivado target part")
    parser.add_argument(
        "--prefer-pipelined-mul-dsp",
        action="store_true",
        help="request the manifest-owned two-cycle Artix-7 Mul_DSP implementation",
    )
    parser.add_argument("--targets", nargs="+", default=[], help="explicit vhls-cli targets to run")
    parser.add_argument("--run-simulation", action="store_true", help="run verify_verilog even if --targets omits it")
    parser.add_argument("--run-synthesis", action="store_true", help="run vivado_synth even if --targets omits it")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--no-dse",
        action="store_true",
        help="disable automatic typed pipeline and external-BRAM directives",
    )
    return parser.parse_args(argv)


def selected_benchmarks(args: argparse.Namespace) -> set[str] | None:
    """Merge repeated benchmark selection options."""

    selected = set(args.benchmarks or []) | set(args.bench or [])
    return selected or None


def effective_targets(args: argparse.Namespace) -> list[str]:
    """Return ordered OpenHLS targets required by the selected options.

    Role:
    Keep the explicit user target list while ensuring ``--run-simulation`` and
    ``--run-synthesis`` request the correct runtime and Vivado synthesis targets.

    Goal:
    Avoid treating schedule-only targets as cycle or area data sources.
    """

    targets = list(args.targets or [])
    if args.run_simulation and "verify_verilog" not in targets:
        targets.append("verify_verilog")
    if args.run_synthesis and "vivado_synth" not in targets:
        targets.append("vivado_synth")
    ordered: list[str] = []
    for target in targets:
        if target not in ordered:
            ordered.append(target)
    return ordered


def discover_jobs(work_root: Path, selected: set[str] | None, args: argparse.Namespace) -> tuple[list[OpenHLSJob], list[OpenHLSResult]]:
    """Stage selected kernels into OpenHLS work directories.

    Role:
    Read each kernel's ``ohls_project.json``, copy source files into an isolated
    work directory, generate a strict ``vhls_project.json`` manifest, and return
    runnable jobs.

    Goal:
    Let OpenHLS operate on all migrated kernels without mutating
    the configured benchmark source tree or relying on the external VibeHLS test
    tree.
    """

    jobs: list[OpenHLSJob] = []
    skipped: list[OpenHLSResult] = []
    for kernel_dir in sorted(path for path in KERNELS_DIR.iterdir() if path.is_dir()):
        if selected is not None and kernel_dir.name not in selected:
            continue
        source_dir = kernel_dir / "src"
        project_path = source_dir / "ohls_project.json"
        if not project_path.is_file():
            skipped.append(skipped_result(kernel_dir.name, "missing ohls_project.json"))
            continue
        try:
            metadata = json.loads(project_path.read_text(encoding="utf-8"))
            job = stage_job(kernel_dir.name, source_dir, metadata, work_root, args)
        except Exception as exc:
            skipped.append(skipped_result(kernel_dir.name, str(exc)))
            continue
        jobs.append(job)

    if selected is not None:
        found = {job.name for job in jobs} | {result.name for result in skipped}
        for missing in sorted(selected - found):
            skipped.append(skipped_result(missing, "benchmark directory not found"))
    return jobs, skipped


def stage_job(name: str, source_dir: Path, metadata: dict[str, Any], work_root: Path, args: argparse.Namespace) -> OpenHLSJob:
    """Create one staged OpenHLS project directory.

    Role:
    Copy all source/configuration files, generate a shared-input harness when one
    exists for this benchmark, and write the ``vhls_project.json`` consumed by
    ``vhls-cli``.

    Goal:
    Keep OpenHLS's generated ``output`` and ``.logs`` trees separate from the
    canonical sources while preserving comparable inputs for the four benchmark
    kernels used in cross-tool sweeps.
    """

    top = str(metadata.get("top_function") or "").strip()
    kernel = str(metadata.get("kernel") or "").strip()
    testbench = metadata.get("testbench") or []
    if not top:
        raise ValueError("ohls_project.json missing top_function")
    if not kernel:
        raise ValueError("ohls_project.json missing kernel")
    if isinstance(testbench, str):
        testbench_list = [testbench]
    elif isinstance(testbench, list):
        testbench_list = [str(item) for item in testbench]
    else:
        testbench_list = []

    # RV32I's native testbench models its architectural 16 MiB address space.
    # The generic shared fixture has only 1,024 entries per memory and would
    # violate that contract during speculative accesses.
    shared = None if name == "rv32i" else common_inputs.get_benchmark_inputs(name)
    excluded_sources = set(testbench_list) if shared is not None else set()
    work_dir = work_root / name
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    copy_source_tree(source_dir, work_dir, excluded_sources)
    kernel_path = work_dir / kernel
    if name == "rv32i" and kernel_path.is_file():
        # RV32I's printf instruction trace is useful in the native reference
        # program but makes OpenHLS lower host I/O into the hardware path.  Keep
        # the canonical source untouched and disable tracing only in this staged
        # synthesis copy; the GCD instruction image and memory interface remain
        # exactly the same.
        kernel_path.write_text(
            "#define NOTRACE 1\n" + kernel_path.read_text(encoding="utf-8"),
            encoding="utf-8",
        )
    loop_entries = loop_labels.label_c_source_file(kernel_path) if kernel_path.is_file() else []

    if shared is not None:
        tb_suffix = ".cpp" if Path(kernel).suffix in {".cpp", ".cc", ".cxx"} else ".c"
        tb_name = f"main_openhls{tb_suffix}"
        (work_dir / tb_name).write_text(common_inputs.generate_c_harness(shared, use_dynamatic_call=False, write_output_file=True), encoding="utf-8")
        (work_dir / "cyclesim.input.json").write_text(
            json.dumps(common_inputs.generate_cyclesim_input(shared), indent=2) + "\n",
            encoding="utf-8",
        )
        testbench_list = [tb_name]

    for required in [kernel, *testbench_list]:
        if required and not (work_dir / required).is_file():
            raise FileNotFoundError(f"required OpenHLS source not found after staging: {required}")

    project_name = str(metadata.get("project_name") or name)
    dse_payload = None if args.no_dse else build_benchmark_dse(
        kernel_path,
        top,
        loop_entries,
        args.llvm_bin,
        work_dir,
        Path(args.vhls_cli).resolve().parents[2],
    )
    manifest = {
        "project_name": project_name,
        "top_function": top,
        "kernel": kernel,
        "testbench": testbench_list,
        "target_fpga": args.part,
        "target_clock_period_ps": max(1, int(round(args.clock_period * 1000.0))),
        "prefer_pipelined_mul_dsp": bool(args.prefer_pipelined_mul_dsp),
        "llvm_bin": str(args.llvm_bin),
        "synthesis": {
            "mode": "local",
            "vivado_bin": args.vivado,
        },
    }
    # Preserve source-selected lowering policies.  These affect the scheduled
    # memory representation, so omitting them can change both legality and QoR.
    for option in (
        # Benchmark-owned preprocessor variants are semantic inputs, not host
        # build flags. In particular, TrueComet selects its single packed-BRAM
        # memcpy transaction only for the OpenHLS compilation variant.
        "compile_defines",
        "bitfield_reconstruction",
        "speculative_loads",
        "array_as_value",
        "array_value_memory_defaults",
        # Preserve explicit global resynthesis policy in staged projects. The
        # runner must not turn a source-selected verification mode into the
        # default proof-on behavior while rebuilding the manifest.
        "global_comb_resynthesis",
        "global_comb_equivalence_check",
    ):
        if option in metadata:
            manifest[option] = metadata[option]
    if dse_payload is not None:
        manifest["dse"] = dse_payload
    manifest_path = work_dir / "vhls_project.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return OpenHLSJob(name, source_dir, work_dir, manifest_path, project_name, top, project_name, dse_payload)


def build_benchmark_dse(
    kernel_path: Path,
    top_function: str,
    loop_entries: list[loop_labels.LoopLabel],
    llvm_bin: str,
    work_dir: Path,
    openhls_root: Path,
) -> dict[str, Any]:
    """Build typed OpenHLS directives for the staged benchmark kernel.

    Role:
    Convert the shared deterministic loop labels and the kernel's pointer/array
    parameters into the public OpenHLS DSE schema.

    Goal:
    Make the benchmark flow enable pipelining only for innermost loops and apply
    a fixed-latency external BRAM contract through typed LLVM metadata, without
    requesting a benchmark-level initiation interval or adding source pragmas.
    The BRAM contract is one read port plus one write port per array argument,
    matching the common single-access-per-array-per-cycle benchmark-memory
    baseline.
    """

    if str(openhls_root) not in sys.path:
        sys.path.insert(0, str(openhls_root))
    from ohls_flow.dse import DseSpec, LoopDirective, LoopSelector

    array_arguments = discover_array_arguments(kernel_path, top_function, llvm_bin, work_dir, openhls_root)
    # OpenHLS hyperblock lowering supports pipeline intent only on innermost
    # loops. Keep every leaf-depth loop in each nest, including sibling leaves,
    # while preventing an outer loop such as SPMV's row traversal from being
    # incorrectly forced through the single-block hyperblock conversion.
    deepest_loop_depth = {
        nest_id: max(entry.nest_depth for entry in loop_entries if entry.nest_id == nest_id)
        for nest_id in {entry.nest_id for entry in loop_entries}
    }
    loops = tuple(
        LoopDirective(
            selector=LoopSelector(function=top_function, label=entry.label),
            pipeline_enable=True,
        )
        for entry in loop_entries
        if entry.nest_depth == deepest_loop_depth[entry.nest_id]
    )
    interfaces = tuple(
        {
            "function": top_function,
            "argument": variable,
            "kind": "external_bram",
            "latency": 1,
            "read_ports": 1,
            "write_ports": 1,
        }
        for variable in array_arguments
    )
    # External-BRAM directives own all argument-memory timing and port choices.
    # Do not add legacy resource directives: they describe local RAM allocation
    # and conflict with the external-memory contract in current OpenHLS.
    payload = DseSpec(loops=loops, interfaces=interfaces).to_dict()
    # The typed file schema is retained in the generated provenance artifact, but
    # the manifest parser accepts only the four directive groups.
    payload.pop("schema", None)
    return payload


def discover_array_arguments(
    kernel_path: Path,
    top_function: str,
    llvm_bin: str,
    work_dir: Path,
    openhls_root: Path,
) -> list[str]:
    """Discover pointer/array parameters of the top kernel through Clang's AST.

    Role:
    Query the selected OpenHLS-compatible Clang frontend instead of parsing LLVM
    IR or guessing from source text.

    Goal:
    Apply RAM directives to every array-like kernel argument, including arrays that
    have decayed to pointers in the C function type.
    """

    clang = Path(llvm_bin) / "clang"
    if not clang.is_file():
        raise FileNotFoundError(f"OpenHLS Clang frontend not found: {clang}")
    language = "c++" if kernel_path.suffix in {".cc", ".cpp", ".cxx"} else "c"
    command = [
        str(clang),
        "-x",
        language,
        "-Xclang",
        "-ast-dump=json",
        "-fsyntax-only",
        "-I",
        str(work_dir),
        "-I",
        str(openhls_root / "library" / "include"),
        str(kernel_path),
    ]
    proc = subprocess.run(command, cwd=work_dir, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"Clang AST discovery failed for {kernel_path.name}: {proc.stderr.strip()}")
    try:
        ast = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Clang AST discovery returned invalid JSON for {kernel_path.name}") from exc

    def walk(node: object):
        if not isinstance(node, dict):
            return
        if node.get("kind") == "FunctionDecl" and node.get("name") == top_function:
            yield node
        for child in node.get("inner", []):
            yield from walk(child)

    declarations = list(walk(ast))
    definition = next(
        (
            item
            for item in declarations
            if item.get("isThisDeclarationADefinition")
            or any(isinstance(child, dict) and child.get("kind") == "CompoundStmt" for child in item.get("inner", []))
        ),
        None,
    )
    if definition is None:
        raise RuntimeError(f"top function definition not found by Clang AST: {top_function}")
    variables: list[str] = []
    for child in definition.get("inner", []):
        if not isinstance(child, dict) or child.get("kind") != "ParmVarDecl":
            continue
        name = child.get("name")
        qual_type = str((child.get("type") or {}).get("qualType", ""))
        if isinstance(name, str) and name and "*" in qual_type:
            variables.append(name)
    return list(dict.fromkeys(variables))


def copy_source_tree(source_dir: Path, work_dir: Path, excluded_sources: set[str]) -> None:
    """Copy source/config files needed by OpenHLS into a staging directory.

    Role:
    Preserve kernel sources, headers, and local configuration files while allowing
    the shared-input harness to replace benchmark-local testbench sources.

    Goal:
    Prevent duplicate ``main`` definitions during VibeHLS RTL runtime builds
    without dropping kernel code or changing the benchmark inputs.
    """

    ignored_dirs = {"output", ".logs", "cfgpre-dot", "hbplan-dot", "__pycache__"}
    for path in source_dir.rglob("*"):
        rel = path.relative_to(source_dir)
        if any(part in ignored_dirs for part in rel.parts):
            continue
        if str(rel) in excluded_sources:
            continue
        if path.is_dir():
            continue
        target = work_dir / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def run_job(job: OpenHLSJob, args: argparse.Namespace) -> OpenHLSResult:
    """Run OpenHLS targets for one staged benchmark.

    Role:
    Execute ``vhls-cli clean`` once, then run each requested target in order,
    collecting per-target logs and return status.

    Goal:
    Produce exact runtime cycle artifacts and Vivado post-synthesis reports in a
    single output tree without later targets deleting earlier results.
    """

    targets = effective_targets(args)
    logs: dict[str, str] = {}
    target_status: dict[str, str] = {}
    if args.dry_run:
        for target in targets:
            logs[target] = str(log_path(job, target))
            target_status[target] = "DRY-RUN"
        return OpenHLSResult(job.name, "DRY-RUN", None, None, job.top_function, str(job.work_dir), str(job.manifest_path), target_status, logs)

    started = time.monotonic()
    clean_rc = run_target(job, args, "clean", logs, target_status)
    if clean_rc != 0:
        return finish_result(job, started, clean_rc, target_status, logs, f"OpenHLS clean failed with exit code {clean_rc}")

    returncode = 0
    message = None
    for target in targets:
        rc = run_target(job, args, target, logs, target_status)
        if rc != 0:
            returncode = rc
            message = f"OpenHLS target {target} failed with exit code {rc}"
            break

    return finish_result(job, started, returncode, target_status, logs, message)


def run_target(job: OpenHLSJob, args: argparse.Namespace, target: str, logs: dict[str, str], target_status: dict[str, str]) -> int:
    """Run one ``vhls-cli`` target and capture stdout/stderr."""

    path = log_path(job, target)
    path.parent.mkdir(parents=True, exist_ok=True)
    command = [args.vhls_cli, target, "--project", str(job.manifest_path)]
    if target != "clean":
        command.extend(["--timeout", f"{args.timeout:g}"])
    logs[target] = str(path)
    with path.open("w", encoding="utf-8") as log:
        log.write("# command: " + " ".join(command) + "\n\n")
        log.flush()
        try:
            proc = subprocess.run(command, cwd=job.work_dir, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout, env=openhls_environment(args))
        except subprocess.TimeoutExpired:
            target_status[target] = "TIMEOUT"
            log.write(f"\n# timed out after {args.timeout:g}s\n")
            return 124
    target_status[target] = "PASS" if proc.returncode == 0 else f"FAIL({proc.returncode})"
    return int(proc.returncode)


def finish_result(job: OpenHLSJob, started: float, returncode: int | None, target_status: dict[str, str], logs: dict[str, str], message: str | None) -> OpenHLSResult:
    """Build the final native result after target execution.

    Role:
    Collect exact cycle and Vivado artifacts after ``ohls-cli`` completes, then
    validate that synthesis did not silently accept incomplete RTL.

    Goal:
    Prevent unresolved floating-point or other external operator black boxes from
    being reported as valid post-synthesis QoR just because Vivado produced a
    utilization report for the remaining connected logic.
    """

    cycles, cycle_source = load_best_cycles(job)
    vivado_summary = vivado_summary_path(job)
    invalid_artifacts = validate_vivado_synthesis_artifacts(job) if vivado_summary.is_file() else []
    has_failure = returncode not in {None, 0}
    has_invalid_artifacts = bool(invalid_artifacts)
    status = "FAIL" if has_failure or has_invalid_artifacts else "PASS"
    if has_invalid_artifacts and message is None:
        message = "; ".join(invalid_artifacts)
    return OpenHLSResult(
        name=job.name,
        status=status,
        returncode=returncode if not has_invalid_artifacts else 1,
        elapsed_seconds=time.monotonic() - started,
        top_function=job.top_function,
        work_dir=str(job.work_dir),
        manifest=str(job.manifest_path),
        target_status=target_status,
        logs=logs,
        latency_cycles=cycles,
        latency_source=str(cycle_source) if cycle_source else None,
        vivado_summary=str(vivado_summary) if vivado_summary.is_file() else None,
        invalid_artifacts=invalid_artifacts or None,
        message=message,
        dse=job.dse,
    )


def vivado_synthesis_log_path(job: OpenHLSJob) -> Path:
    """Return the Vivado batch synthesis log path for one OpenHLS job.

    Role:
    Encode the OpenHLS Vivado staging layout used by the ``vivado_synth`` target.

    Goal:
    Let result validation inspect the same synthesis transcript that produced the
    compact ``*.vivado.summary.json`` metrics.
    """

    return job.work_dir / "output" / "synth" / "vivado" / "stage" / "reports" / f"{job.output_base}.vivado.synth.log"


def validate_vivado_synthesis_artifacts(job: OpenHLSJob) -> list[str]:
    """Detect Vivado synthesis artifacts that cannot represent complete hardware.

    Role:
    Scan the Vivado synthesis transcript for unresolved non-primitive black boxes
    left by OpenHLS-generated RTL modules such as external floating-point
    operators.

    Goal:
    Mark incomplete synthesized netlists as failed/invalid instead of publishing
    resource numbers for hardware with missing operator implementations.
    """

    log = vivado_synthesis_log_path(job)
    if not log.is_file():
        return []
    try:
        text = log.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"could not read Vivado synthesis log {log}: {exc}"]
    marker = "Could not resolve non-primitive black box cell"
    if marker not in text:
        return []
    count = text.count(marker)
    return [f"Vivado synthesis left {count} unresolved non-primitive black-box cell(s); see {log}"]


def log_path(job: OpenHLSJob, target: str) -> Path:
    """Return the runner-owned log path for one target."""

    return job.work_dir / ".logs" / f"{target}.stdout.log"


def openhls_environment(args: argparse.Namespace) -> dict[str, str]:
    """Return the environment used for ``vhls-cli`` subprocesses.

    Role:
    Recreate the important exports from the local OpenHLS ``settings.sh`` while
    also exposing the selected Vivado executable for synthesis targets.

    Goal:
    Avoid system ``clang`` loading OpenHLS plugins with an incompatible LLVM ABI
    and keep direct ``ohls-cli`` execution reproducible from the runner logs.
    """

    env = os.environ.copy()
    openhls_root = Path(args.vhls_cli).resolve().parents[2]
    llvm22_install = Path.home() / ".local" / "llvm22"
    llvm22_build = Path(args.llvm_bin).resolve().parent
    mlir_build = openhls_root / "frontend" / "mlir" / "build-ohls"
    llvm_frontend_bin = openhls_root / "frontend" / "llvm" / "build" / "bin"
    llvm_bin = str(args.llvm_bin)
    circt_bin = str(args.circt_bin)
    vivado_path = Path(args.vivado)
    path_entries = [
        str(mlir_build),
        str(llvm_frontend_bin),
        circt_bin,
        llvm_bin,
        str(openhls_root / "tools" / "bin"),
    ]
    if vivado_path.parent.is_dir():
        path_entries.append(str(vivado_path.parent))
    lib_entries = [str(llvm22_install / "lib"), str(llvm22_build / "lib")]
    env["CIRCT_BIN"] = circt_bin
    env["OHLS_OPT"] = str(mlir_build / "ohls-opt")
    env["LLVM_DIR"] = str(llvm22_install / "lib" / "cmake" / "llvm")
    env["Clang_DIR"] = str(llvm22_install / "lib" / "cmake" / "clang")
    env["CLANG_DIR"] = env["Clang_DIR"]
    env["MLIR_DIR"] = str(llvm22_install / "lib" / "cmake" / "mlir")
    env["CIRCT_DIR"] = str(llvm22_install / "lib" / "cmake" / "circt")
    env["PYTHONPATH"] = f"{openhls_root}{os.pathsep}{env.get('PYTHONPATH', '')}" if env.get("PYTHONPATH") else str(openhls_root)
    env["LD_LIBRARY_PATH"] = os.pathsep.join(lib_entries + ([env["LD_LIBRARY_PATH"]] if env.get("LD_LIBRARY_PATH") else []))
    env["PATH"] = os.pathsep.join(path_entries + [env.get("PATH", "")])
    return env


def runtime_json_path(job: OpenHLSJob, level: str) -> Path:
    """Return the expected OpenHLS runtime JSON path for one cycle level."""

    if level == "rtl":
        return job.work_dir / "output" / "rtl" / "runtime" / f"{job.output_base}.verify_rtl.runtime.json"
    if level == "L3":
        return job.work_dir / "output" / "cyclesim" / "L3" / f"{job.output_base}.cyclesim.l3.compiled.runtime.json"
    if level == "L2":
        return job.work_dir / "output" / "cyclesim" / "L2" / f"{job.output_base}.cyclesim.l2.compiled.runtime.json"
    raise ValueError(level)


def load_best_cycles(job: OpenHLSJob) -> tuple[int | None, Path | None]:
    """Load exact cycle count, preferring RTL over L3 and L2 runtime JSON."""

    for level in ("rtl", "L3", "L2"):
        path = runtime_json_path(job, level)
        cycles = load_cycle_json(path)
        if cycles is not None:
            return cycles, path
    return None, None


def load_cycle_json(path: Path) -> int | None:
    """Parse an OpenHLS runtime JSON cycle count if available."""

    if not path.is_file():
        return None
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    value = payload.get("cycle_count")
    return value if isinstance(value, int) else None


def vivado_summary_path(job: OpenHLSJob) -> Path:
    """Return the compact Vivado summary JSON path emitted by OpenHLS."""

    return job.work_dir / "output" / "synth" / "vivado" / f"{job.output_base}.vivado.summary.json"


def parse_vivado_summary(path: Path) -> dict[str, Any]:
    """Parse OpenHLS Vivado post-synthesis metrics from summary JSON."""

    metrics = empty_metrics()
    if not path.is_file():
        return metrics
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return metrics
    metrics.update(
        {
            "lut": to_int(payload.get("LUT")),
            "ff": to_int(payload.get("FF")),
            "bram": to_int(payload.get("BRAM")),
            "dsp": to_int(payload.get("DSP")),
            "tclk_ns": (to_int(payload.get("Tclk_est_ps")) / 1000.0 if to_int(payload.get("Tclk_est_ps")) is not None else None),
            "slack_ns": to_float(payload.get("WNS_ns")),
        }
    )
    return metrics


def empty_metrics() -> dict[str, Any]:
    """Return the common empty metric object."""

    return {"area": None, "lut": None, "ff": None, "bram": None, "bram_bits": None, "dsp": None, "tclk_ns": None, "fmax_mhz": None, "slack_ns": None}


def to_int(value: Any) -> int | None:
    """Convert JSON values to int when possible."""

    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    try:
        return int(str(value))
    except Exception:
        return None


def to_float(value: Any) -> float | None:
    """Convert JSON values to float when possible."""

    if isinstance(value, (int, float)):
        return float(value)
    try:
        return float(str(value))
    except Exception:
        return None


def write_native_summary(path: Path, args: argparse.Namespace, results: list[OpenHLSResult]) -> None:
    """Write the OpenHLS-native JSON summary."""

    payload = {
        "config": {
            "clock_period_ns": args.clock_period,
            "part": args.part,
            "targets": effective_targets(args),
            "timeout": args.timeout,
            "vhls_cli": args.vhls_cli,
            "vivado": args.vivado,
            "llvm_bin": str(args.llvm_bin),
            "circt_bin": str(args.circt_bin),
            "work_root": str(args.work_root),
            "automatic_dse": not args.no_dse,
            "prefer_pipelined_mul_dsp": args.prefer_pipelined_mul_dsp,
        },
        "results": [result.__dict__ for result in results],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_normalized_summary(path: Path, args: argparse.Namespace, results: list[OpenHLSResult]) -> None:
    """Write normalized QoR JSON for OpenHLS results.

    Role:
    Convert native OpenHLS run records into the repository-wide QoR schema.

    Goal:
    Publish Vivado metrics only for complete hardware artifacts; if validation
    found unresolved black boxes, keep the failure visible and leave QoR fields
    empty rather than reporting misleading partial-netlist resource numbers.
    """

    records = []
    for result in results:
        post_synth = empty_metrics() if result.invalid_artifacts else (parse_vivado_summary(Path(result.vivado_summary)) if result.vivado_summary else empty_metrics())
        records.append(
            {
                "benchmark": result.name,
                "status": "ok" if result.status == "PASS" else ("skipped" if result.status == "SKIP" else "failed"),
                "message": result.message,
                "returncode": result.returncode,
                "elapsed_seconds": result.elapsed_seconds,
                "tool": "openhls",
                "target": {"clock_period_ns": args.clock_period, "part": args.part},
                "sources": {"top_function": result.top_function},
                "latency": {"cycles": result.latency_cycles, "source": result.latency_source},
                "tool_estimate": empty_metrics(),
                "post_synth": post_synth,
                "artifacts": {
                    "work_dir": result.work_dir,
                    "manifest": result.manifest,
                    "logs": result.logs,
                    "vivado_summary": result.vivado_summary,
                    "invalid_artifacts": result.invalid_artifacts,
                    "dse": result.dse,
                },
            }
        )
    common_qor.write_json(
        path,
        common_qor.build_report(
            tool="openhls",
            config={"clock_period_ns": args.clock_period, "part": args.part, "vhls_cli": args.vhls_cli, "vivado": args.vivado, "llvm_bin": str(args.llvm_bin), "circt_bin": str(args.circt_bin), "automatic_dse": not args.no_dse, "prefer_pipelined_mul_dsp": args.prefer_pipelined_mul_dsp},
            benchmarks=records,
        ),
    )


def skipped_result(name: str, message: str) -> OpenHLSResult:
    """Create a skipped result for discovery/staging failures."""

    return OpenHLSResult(name, "SKIP", None, None, None, None, None, {}, {}, message=message)


def print_summary(results: list[OpenHLSResult]) -> None:
    """Print a compact OpenHLS run table.

    Role:
    Display the same validity decision used for normalized reports.

    Goal:
    Avoid showing invalid post-synthesis metrics interactively when Vivado accepted
    incomplete RTL with unresolved black boxes.
    """

    headers = ["benchmark", "status", "top", "cycles", "LUT", "FF", "BRAM", "DSP", "message"]
    rows = []
    for result in results:
        metrics = empty_metrics() if result.invalid_artifacts else (parse_vivado_summary(Path(result.vivado_summary)) if result.vivado_summary else empty_metrics())
        rows.append([
            result.name,
            result.status,
            result.top_function or "-",
            str(result.latency_cycles) if result.latency_cycles is not None else "-",
            str(metrics.get("lut")) if metrics.get("lut") is not None else "-",
            str(metrics.get("ff")) if metrics.get("ff") is not None else "-",
            str(metrics.get("bram")) if metrics.get("bram") is not None else "-",
            str(metrics.get("dsp")) if metrics.get("dsp") is not None else "-",
            result.message or "",
        ])
    widths = [max(len(row[index]) for row in [headers, *rows]) for index in range(len(headers))]
    print(" | ".join(headers[index].ljust(widths[index]) for index in range(len(headers))))
    print("-+-".join("-" * width for width in widths))
    for row in rows:
        print(" | ".join(row[index].ljust(widths[index]) for index in range(len(headers))))


if __name__ == "__main__":
    raise SystemExit(main())
