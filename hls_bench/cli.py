#!/usr/bin/env python3
"""Single command-line entry point for local HLS benchmark tools.

Role:
Provide one repo-level CLI for LegUp, Bambu/PANDA, Dynamatic, Vitis HLS, and
OpenHLS/VibeHLS benchmark runs.

Goal:
Keep the existing per-tool scripts usable while offering a consistent front door
for discovery, execution, and normalized QoR report generation.

How:
LegUp and Bambu subcommands delegate to their current runners so their behavior
and native reports remain unchanged.  The Dynamatic subcommand uses the shared
Dynamatic runner implemented in ``hls_bench.dynamatic_runner`` because there was
no existing repository runner for the local ``~/.local`` installation.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

from hls_bench import dynamatic_runner, openhls_runner, parallel_runner, paths, vitis_runner


REPO_ROOT = Path(__file__).resolve().parents[1]
LEGUP_RUNNER = REPO_ROOT / "hls_bench" / "legup_runner.py"
BAMBU_RUNNER = REPO_ROOT / "osshls_bench" / "tools" / "panda" / "run.py"
DEFAULT_TCLK_NS = 10.0


def main(argv: list[str] | None = None) -> int:
    """Parse the top-level CLI and dispatch to one tool command.

    Role:
    Route user requests to LegUp, Bambu, Dynamatic, or a sequential multi-tool
    run.

    Goal:
    Make all benchmark flows reachable from one command without hiding each
    tool's detailed options.
    """

    parser = argparse.ArgumentParser(description="Run HLS benchmark tools from one CLI.", allow_abbrev=False)
    parser.add_argument("--Tclk", type=float, default=DEFAULT_TCLK_NS, help="target clock period in ns")
    subparsers = parser.add_subparsers(dest="command", required=True)

    _add_passthrough(subparsers, "legup", "Run the existing LegUp benchmark runner")
    _add_passthrough(subparsers, "bambu", "Run the existing Bambu/PANDA benchmark runner")
    dynamatic_parser = subparsers.add_parser("dynamatic", help="Run local Dynamatic benchmark flow", allow_abbrev=False)
    _add_dynamatic_options(dynamatic_parser)
    vitis_parser = subparsers.add_parser("vitis", help="Run local Vitis HLS benchmark flow", allow_abbrev=False)
    _add_vitis_options(vitis_parser)
    openhls_parser = subparsers.add_parser("openhls", help="Run local OpenHLS/VibeHLS benchmark flow", allow_abbrev=False)
    _add_openhls_options(openhls_parser)

    all_parser = subparsers.add_parser("all", help="Run LegUp, Bambu, then Dynamatic sequentially", allow_abbrev=False)
    all_parser.add_argument("--Tclk", type=float, default=argparse.SUPPRESS, help="target clock period in ns for every tool")
    all_parser.add_argument("--benchmarks", action="append", help="benchmark to pass to every tool; may be repeated")
    all_parser.add_argument("--skip-legup", action="store_true")
    all_parser.add_argument("--skip-bambu", action="store_true")
    all_parser.add_argument("--skip-dynamatic", action="store_true")
    all_parser.add_argument("--legup-run-simulation", action="store_true")
    all_parser.add_argument("--bambu-run-simulation", action="store_true")
    all_parser.add_argument("--bambu-run-synthesis", action="store_true")
    all_parser.add_argument("--dynamatic-run-simulation", action="store_true")
    all_parser.add_argument("--dynamatic-run-synthesis", action="store_true")
    all_parser.add_argument("--dynamatic-part", default="xc7a100tcsg324-1")

    tools_parser = subparsers.add_parser("tools", help="List configured benchmark tools", allow_abbrev=False)
    tools_parser.set_defaults(func=_cmd_tools)

    parallel_parser = subparsers.add_parser("parallel", help="Run tool/benchmark jobs in parallel", allow_abbrev=False)
    _add_parallel_options(parallel_parser)

    args, extras = parser.parse_known_args(argv)
    if args.command == "legup":
        child_args = _with_tclk(_strip_separator([*args.args, *extras]), ["--period"], "--period", args.Tclk)
        return _run_script(LEGUP_RUNNER, child_args)
    if args.command == "bambu":
        child_args = _with_tclk(_strip_separator([*args.args, *extras]), ["--clock-period"], "--clock-period", args.Tclk)
        return _run_script(BAMBU_RUNNER, child_args)
    if args.command == "dynamatic":
        child_args = _build_dynamatic_child_args(args, extras)
        return dynamatic_runner.main(child_args)
    if args.command == "vitis":
        child_args = _build_vitis_child_args(args, extras)
        return vitis_runner.main(child_args)
    if args.command == "openhls":
        child_args = _build_openhls_child_args(args, extras)
        return openhls_runner.main(child_args)
    if args.command == "all":
        return _cmd_all(args)
    if args.command == "parallel":
        return parallel_runner.main(_build_parallel_child_args(args, extras))
    return args.func(args)


def _add_passthrough(subparsers: argparse._SubParsersAction, name: str, help_text: str) -> None:
    """Register a subcommand that forwards remaining arguments to a script.

    Role:
    Preserve each existing runner's CLI without re-declaring every option at the
    top level.

    Goal:
    Let users call commands such as ``python -m hls_bench.cli bambu -- --benchmarks gcd``
    while keeping parser conflicts isolated from the wrapper CLI.
    """

    parser = subparsers.add_parser(name, help=help_text, allow_abbrev=False)
    parser.add_argument("--Tclk", type=float, default=argparse.SUPPRESS, help="target clock period in ns")
    parser.add_argument("args", nargs=argparse.REMAINDER, help=f"arguments passed to {name}")


def _add_dynamatic_options(parser: argparse.ArgumentParser) -> None:
    """Register the native Dynamatic runner options on the wrapper CLI."""

    parser.add_argument("--Tclk", type=float, default=argparse.SUPPRESS, help="target clock period in ns")
    parser.add_argument("--benchmarks", nargs="+", help="benchmark directory names to run")
    parser.add_argument("--bench", action="append", help="single benchmark name; may be repeated")
    parser.add_argument("--dynamatic-root", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--work-root", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--summary", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--normalized-summary", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--timeout", type=float, default=argparse.SUPPRESS)
    parser.add_argument("--part", default=argparse.SUPPRESS)
    parser.add_argument("--simple-buffers", action="store_true")
    parser.add_argument("--fp-units-generator", choices=["flopoco", "vivado"], default=argparse.SUPPRESS)
    parser.add_argument("--simulator", choices=["verilator", "xsim", "ghdl", "vsim"], default=argparse.SUPPRESS)
    parser.add_argument("--run-simulation", action="store_true")
    parser.add_argument("--run-synthesis", action="store_true")
    parser.add_argument("--dry-run", action="store_true")


def _add_parallel_options(parser: argparse.ArgumentParser) -> None:
    """Register options for the built-in parallel scheduler.

    Role:
    Expose benchmark/tool selection, concurrency, timeout, and executable paths
    through the top-level CLI without duplicating scheduler implementation here.

    Goal:
    Keep ``python -m hls_bench.cli parallel`` convenient for multi-tool QoR sweeps while
    forwarding the exact options to ``hls_bench.parallel_runner``.
    """

    parser.add_argument("--Tclk", type=float, default=argparse.SUPPRESS, help="target clock period in ns")
    parser.add_argument("--benchmarks", nargs="+", default=argparse.SUPPRESS, help="benchmark names to run")
    parser.add_argument("--bench", action="append", help="single benchmark name; may be repeated")
    parser.add_argument("--tools", nargs="+", choices=["legup", "bambu", "dynamatic", "vitis", "openhls"], default=argparse.SUPPRESS)
    parser.add_argument("--jobs", type=int, default=argparse.SUPPRESS)
    parser.add_argument("--timeout", type=float, default=argparse.SUPPRESS, help="per-job timeout in seconds; default is 600")
    parser.add_argument("--poll-interval", type=float, default=argparse.SUPPRESS)
    parser.add_argument("--run-root", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--vivado", default=argparse.SUPPRESS)
    parser.add_argument("--bambu", default=argparse.SUPPRESS)
    parser.add_argument("--bambu-device", default=argparse.SUPPRESS)
    parser.add_argument("--dynamatic-part", default=argparse.SUPPRESS)
    parser.add_argument("--dynamatic-fp-units-generator", choices=["flopoco", "vivado"], default=argparse.SUPPRESS)
    parser.add_argument("--dynamatic-simulator", choices=["verilator", "xsim", "ghdl", "vsim"], default=argparse.SUPPRESS)
    parser.add_argument("--vitis-hls", default=argparse.SUPPRESS)
    parser.add_argument("--vitis-part", default=argparse.SUPPRESS)
    parser.add_argument("--vhls-cli", default=argparse.SUPPRESS)
    parser.add_argument("--openhls-part", default=argparse.SUPPRESS)
    parser.add_argument("--openhls-llvm-bin", default=argparse.SUPPRESS)
    parser.add_argument("--openhls-circt-bin", default=argparse.SUPPRESS)
    parser.add_argument("--openhls-no-dse", action="store_true")
    parser.add_argument("--no-simulation", action="store_true")
    parser.add_argument("--no-synthesis", action="store_true")


def _add_vitis_options(parser: argparse.ArgumentParser) -> None:
    """Register the native Vitis HLS runner options on the wrapper CLI."""

    parser.add_argument("--Tclk", type=float, default=argparse.SUPPRESS, help="target clock period in ns")
    parser.add_argument("--benchmarks", nargs="+", help="benchmark directory names to run")
    parser.add_argument("--bench", action="append", help="single benchmark name; may be repeated")
    parser.add_argument("--vitis-hls", default=argparse.SUPPRESS)
    parser.add_argument("--vivado", default=argparse.SUPPRESS)
    parser.add_argument("--work-root", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--summary", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--normalized-summary", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--timeout", type=float, default=argparse.SUPPRESS)
    parser.add_argument("--part", default=argparse.SUPPRESS)
    parser.add_argument("--run-simulation", action="store_true")
    parser.add_argument("--run-synthesis", action="store_true")
    parser.add_argument("--unsafe-math-optimizations", action="store_true")
    parser.add_argument("--dry-run", action="store_true")


def _add_openhls_options(parser: argparse.ArgumentParser) -> None:
    """Register the native OpenHLS/VibeHLS runner options on the wrapper CLI.

    Role:
    Expose the same benchmark, clock, executable, staging, and report options as
    ``hls_bench.openhls_runner`` while keeping the top-level CLI responsible only
    for argument translation.

    Goal:
    Make ``python -m hls_bench.cli openhls`` behave like the other first-class tool
    subcommands without duplicating runner implementation.
    """

    parser.add_argument("--Tclk", type=float, default=argparse.SUPPRESS, help="target clock period in ns")
    parser.add_argument("--benchmarks", nargs="+", help="benchmark directory names to run")
    parser.add_argument("--bench", action="append", help="single benchmark name; may be repeated")
    parser.add_argument("--vhls-cli", default=argparse.SUPPRESS)
    parser.add_argument("--vivado", default=argparse.SUPPRESS)
    parser.add_argument("--llvm-bin", default=argparse.SUPPRESS)
    parser.add_argument("--circt-bin", default=argparse.SUPPRESS)
    parser.add_argument("--work-root", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--summary", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--normalized-summary", type=Path, default=argparse.SUPPRESS)
    parser.add_argument("--timeout", type=float, default=argparse.SUPPRESS)
    parser.add_argument("--part", default=argparse.SUPPRESS)
    parser.add_argument("--targets", nargs="+", default=argparse.SUPPRESS)
    parser.add_argument("--run-simulation", action="store_true")
    parser.add_argument("--run-synthesis", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--prefer-pipelined-mul-dsp", action="store_true")


def _build_dynamatic_child_args(args: argparse.Namespace, extras: list[str]) -> list[str]:
    """Translate wrapper options into the Dynamatic runner's argv list."""

    child: list[str] = []
    tclk = getattr(args, "Tclk", DEFAULT_TCLK_NS)
    child.extend(["--clock-period", str(tclk)])
    if getattr(args, "benchmarks", None):
        child.extend(["--benchmarks", *args.benchmarks])
    if getattr(args, "bench", None):
        for bench in args.bench:
            child.extend(["--bench", bench])
    for opt in ("dynamatic_root", "work_root", "summary", "normalized_summary", "timeout", "part", "fp_units_generator", "simulator"):
        if hasattr(args, opt):
            value = getattr(args, opt)
            child.extend([f"--{opt.replace('_', '-')}", str(value)])
    if getattr(args, "simple_buffers", False):
        child.append("--simple-buffers")
    if getattr(args, "run_simulation", False):
        child.append("--run-simulation")
    if getattr(args, "run_synthesis", False):
        child.append("--run-synthesis")
    if getattr(args, "dry_run", False):
        child.append("--dry-run")
    child.extend(extras)
    return child


def _build_parallel_child_args(args: argparse.Namespace, extras: list[str]) -> list[str]:
    """Translate parsed top-level parallel options into scheduler argv.

    Role:
    Convert argparse attributes back to the scheduler's native argument vector so
    the implementation remains testable and callable outside the wrapper.

    Goal:
    Avoid keeping two independent copies of scheduler defaults or option logic.
    """

    child: list[str] = []
    tclk = getattr(args, "Tclk", DEFAULT_TCLK_NS)
    child.extend(["--Tclk", str(tclk)])
    benchmarks = []
    if getattr(args, "benchmarks", None):
        benchmarks.extend(args.benchmarks)
    if getattr(args, "bench", None):
        benchmarks.extend(args.bench)
    if benchmarks:
        child.extend(["--benchmarks", *benchmarks])
    if getattr(args, "tools", None):
        child.extend(["--tools", *args.tools])
    for opt in ("jobs", "timeout", "poll_interval", "run_root", "vivado", "bambu", "bambu_device", "dynamatic_part", "dynamatic_fp_units_generator", "dynamatic_simulator", "vitis_hls", "vitis_part", "vhls_cli", "openhls_part", "openhls_llvm_bin", "openhls_circt_bin"):
        if hasattr(args, opt):
            value = getattr(args, opt)
            child.extend([f"--{opt.replace('_', '-')}", str(value)])
    if getattr(args, "no_simulation", False):
        child.append("--no-simulation")
    if getattr(args, "no_synthesis", False):
        child.append("--no-synthesis")
    if getattr(args, "openhls_no_dse", False):
        child.append("--openhls-no-dse")
    child.extend(extras)
    return child


def _build_vitis_child_args(args: argparse.Namespace, extras: list[str]) -> list[str]:
    """Translate wrapper options into the Vitis HLS runner's argv list."""

    child: list[str] = []
    tclk = getattr(args, "Tclk", DEFAULT_TCLK_NS)
    child.extend(["--clock-period", str(tclk)])
    if getattr(args, "benchmarks", None):
        child.extend(["--benchmarks", *args.benchmarks])
    if getattr(args, "bench", None):
        for bench in args.bench:
            child.extend(["--bench", bench])
    for opt in ("vitis_hls", "vivado", "work_root", "summary", "normalized_summary", "timeout", "part"):
        if hasattr(args, opt):
            value = getattr(args, opt)
            child.extend([f"--{opt.replace('_', '-')}", str(value)])
    if getattr(args, "run_simulation", False):
        child.append("--run-simulation")
    if getattr(args, "run_synthesis", False):
        child.append("--run-synthesis")
    if getattr(args, "unsafe_math_optimizations", False):
        child.append("--unsafe-math-optimizations")
    if getattr(args, "dry_run", False):
        child.append("--dry-run")
    child.extend(extras)
    return child


def _build_openhls_child_args(args: argparse.Namespace, extras: list[str]) -> list[str]:
    """Translate wrapper options into the OpenHLS/VibeHLS runner's argv list.

    Role:
    Convert parsed argparse fields back into the standalone runner's native
    command-line options.

    Goal:
    Preserve a single source of truth for OpenHLS execution while still allowing
    the repository-level wrapper to share the common ``--Tclk`` option.
    """

    child: list[str] = []
    tclk = getattr(args, "Tclk", DEFAULT_TCLK_NS)
    child.extend(["--clock-period", str(tclk)])
    if getattr(args, "benchmarks", None):
        child.extend(["--benchmarks", *args.benchmarks])
    if getattr(args, "bench", None):
        for bench in args.bench:
            child.extend(["--bench", bench])
    for opt in ("vhls_cli", "vivado", "llvm_bin", "circt_bin", "work_root", "summary", "normalized_summary", "timeout", "part"):
        if hasattr(args, opt):
            value = getattr(args, opt)
            child.extend([f"--{opt.replace('_', '-')}", str(value)])
    if hasattr(args, "targets"):
        child.extend(["--targets", *args.targets])
    if getattr(args, "run_simulation", False):
        child.append("--run-simulation")
    if getattr(args, "run_synthesis", False):
        child.append("--run-synthesis")
    if getattr(args, "prefer_pipelined_mul_dsp", False):
        child.append("--prefer-pipelined-mul-dsp")
    if getattr(args, "dry_run", False):
        child.append("--dry-run")
    child.extend(extras)
    return child


def _cmd_tools(_args: argparse.Namespace) -> int:
    """Print available tools and their default report artifacts.

    Role:
    Provide a quick inventory of the configured runners and output paths.

    Goal:
    Make the single CLI self-documenting without requiring users to inspect the
    underlying scripts.
    """

    rows = [
        ["legup", str(LEGUP_RUNNER), str(paths.LEGUP_WORK_ROOT / "legup_qor.json")],
        ["bambu", str(BAMBU_RUNNER), str(paths.BAMBU_WORK_ROOT / "bambu_qor.json")],
        ["dynamatic", "hls_bench.dynamatic_runner", str(paths.DYNAMATIC_WORK_ROOT / "dynamatic_qor.json")],
        ["vitis", "hls_bench.vitis_runner", str(paths.VITIS_WORK_ROOT / "vitis_qor.json")],
        ["openhls", "hls_bench.openhls_runner", str(paths.OPENHLS_WORK_ROOT / "openhls_qor.json")],
    ]
    headers = ["tool", "runner", "normalized_qor"]
    widths = [max(len(row[index]) for row in [headers, *rows]) for index in range(len(headers))]
    print(" | ".join(headers[index].ljust(widths[index]) for index in range(len(headers))))
    print("-+-".join("-" * width for width in widths))
    for row in rows:
        print(" | ".join(row[index].ljust(widths[index]) for index in range(len(headers))))
    return 0


def _cmd_all(args: argparse.Namespace) -> int:
    """Run the supported tools sequentially with a shared benchmark selection.

    Role:
    Translate the top-level ``--bench`` option into each runner's benchmark
    selection syntax and execute the requested tools in a deterministic order.

    Goal:
    Provide a simple aggregate command while keeping advanced per-tool options
    available through the individual subcommands.
    """

    exit_code = 0
    if not args.skip_legup:
        legup_args: list[str] = ["--period", str(args.Tclk)]
        for bench in args.benchmarks or []:
            legup_args.extend(["--bench", bench])
        if args.legup_run_simulation:
            legup_args.append("--run-simulation")
        exit_code = _merge_exit(exit_code, _run_script(LEGUP_RUNNER, legup_args))
    if not args.skip_bambu:
        bambu_args: list[str] = ["--clock-period", str(args.Tclk)]
        if args.benchmarks:
            bambu_args.extend(["--benchmarks", *args.benchmarks])
        if args.bambu_run_simulation:
            bambu_args.append("--run-simulation")
        if args.bambu_run_synthesis:
            bambu_args.append("--run-synthesis")
        exit_code = _merge_exit(exit_code, _run_script(BAMBU_RUNNER, bambu_args))
    if not args.skip_dynamatic:
        dyn_args: list[str] = []
        if args.benchmarks:
            dyn_args.extend(["--benchmarks", *args.benchmarks])
        dyn_args.extend(["--part", args.dynamatic_part])
        dyn_args.extend(["--clock-period", str(args.Tclk)])
        if args.dynamatic_run_simulation:
            dyn_args.append("--run-simulation")
        if args.dynamatic_run_synthesis:
            dyn_args.append("--run-synthesis")
        exit_code = _merge_exit(exit_code, dynamatic_runner.main(dyn_args))
    return exit_code


def _run_script(script: Path, args: list[str]) -> int:
    """Execute a repository Python runner as a child process.

    Role:
    Invoke existing scripts with the current Python interpreter from the repo
    root.

    Goal:
    Avoid importing scripts that were designed as command-line programs while
    still providing a unified command wrapper.
    """

    command = [sys.executable, str(script), *args]
    completed = subprocess.run(command, cwd=REPO_ROOT)
    return completed.returncode


def _strip_separator(args: list[str]) -> list[str]:
    """Remove an optional ``--`` separator from passthrough arguments.

    Role:
    Support both ``python -m hls_bench.cli bambu -- --benchmarks gcd`` and
    ``python -m hls_bench.cli bambu --benchmarks gcd`` invocation styles.

    Goal:
    Keep the wrapper convenient while avoiding accidental forwarding of the
    separator itself to child parsers.
    """

    return args[1:] if args and args[0] == "--" else args


def _with_tclk(args: list[str], option_names: list[str], option_name: str, tclk: float) -> list[str]:
    """Add the wrapper-level target clock period unless the child already has one.

    Role:
    Translate ``python -m hls_bench.cli <tool> --Tclk`` into the tool-specific clock-period
    option while preserving explicit pass-through overrides.

    Goal:
    Keep the single CLI clock setting consistent across tools without producing
    duplicate period arguments when a caller intentionally supplies a native
    child-runner option.
    """

    if _contains_option(args, option_names):
        return args
    return [option_name, str(tclk), *args]


def _contains_option(args: list[str], option_names: list[str]) -> bool:
    """Return whether a pass-through argument list already contains an option.

    Role:
    Detect both ``--option value`` and ``--option=value`` forms.

    Goal:
    Avoid overriding explicit child-runner clock-period settings supplied by the
    user.
    """

    return any(arg == name or arg.startswith(f"{name}=") for arg in args for name in option_names)


def _merge_exit(previous: int, current: int) -> int:
    """Combine sequential tool exit codes.

    Role:
    Preserve the first non-zero status from an aggregate run.

    Goal:
    Let ``all`` continue across tools but still report failure to calling scripts.
    """

    return previous if previous else current



if __name__ == "__main__":
    raise SystemExit(main())
