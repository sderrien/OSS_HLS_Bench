"""Run HLS benchmark tools in parallel and summarize normalized QoR.

Role:
Provide the implementation behind ``python -m hls_bench.cli parallel``.  The module launches
one isolated subprocess per selected ``(tool, benchmark)`` pair, keeps each tool's
JSON/log outputs separate to avoid report-file races, enforces a per-job timeout,
and prints progress as jobs start, finish, fail, or time out.

Goal:
Make multi-tool QoR sweeps practical for the local LegUp, Bambu/PANDA,
Dynamatic, Vitis HLS, and OpenHLS/VibeHLS flows without hand-writing shell loops
or losing partial results when a single benchmark stalls.

How:
Each job invokes the existing tool runner through the repository CLI or script
entry point.  The scheduler uses a small polling loop around ``subprocess.Popen``
so it can maintain a fixed concurrency limit, terminate whole process groups on
timeout, and then read the normalized QoR JSON artifacts to print one ASCII table.
"""

from __future__ import annotations

import argparse
import html
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time
from typing import Any
import xml.etree.ElementTree as ET

from hls_bench import config, loop_labels, paths

REPO_ROOT = paths.REPO_ROOT
CLI = REPO_ROOT / "hls_bench" / "cli.py"
DEFAULT_BENCHMARKS = ["spmv", "dotprod", "matmul", "gcd", "passthrough", "histogram", "iir", "iterative_sqrt", "lbm", "qr_givens", "rv32i", "rv32i_comet"]
DEFAULT_TOOLS = ["legup", "bambu", "dynamatic", "vitis", "openhls"]
SUPPORTED_TOOLS = ["legup", "bambu", "dynamatic", "vitis", "openhls"]
DEFAULT_CLOCK_PERIOD_NS = 10.0
DEFAULT_TIMEOUT_SECONDS = 600.0
DEFAULT_VIVADO = config.VIVADO
DEFAULT_BAMBU = config.BAMBU
DEFAULT_BAMBU_DEVICE = "xc7a100t,-1,csg324,VVD"
DEFAULT_DYNAMATIC_PART = "xc7a100tcsg324-1"
DEFAULT_DYNAMATIC_FP_UNITS_GENERATOR = "vivado"
DEFAULT_DYNAMATIC_SIMULATOR = "xsim"
DEFAULT_VITIS_HLS = config.VITIS_HLS
DEFAULT_VITIS_PART = "xc7a100tcsg324-1"
LOOP_II_TOOLS = ("legup", "bambu", "dynamatic", "vitis", "openhls")
DEFAULT_VHLS_CLI = config.VHLS_CLI
DEFAULT_OPENHLS_PART = "xc7a100tcsg324-1"
DEFAULT_OPENHLS_LLVM_BIN = str(config.OPENHLS_LLVM_BIN)
DEFAULT_OPENHLS_CIRCT_BIN = str(config.OPENHLS_CIRCT_BIN)
ARTIX7_A100T_CAPACITY = {
    "lut": 63400.0,
    "ff": 126800.0,
    "bram": 135.0,
    "dsp": 240.0,
}
TOOL_COLORS = {
    "legup": "#1f77b4",
    "bambu": "#ff7f0e",
    "dynamatic": "#2ca02c",
    "vitis": "#d62728",
    "openhls": "#9467bd",
}


@dataclass
class ParallelJob:
    """Mutable runtime state for one parallel tool/benchmark subprocess.

    Role:
    Carry the command line, artifact paths, process handle, timing data, and final
    status for a single scheduled unit of work.

    Goal:
    Keep scheduling logic simple and make status serialization/reporting avoid
    recomputing paths from tool-specific conventions.
    """

    tool: str
    benchmark: str
    command: list[str]
    log_path: Path
    qor_path: Path
    status: str = "pending"
    returncode: int | None = None
    started_at: float | None = None
    ended_at: float | None = None
    process: subprocess.Popen | None = field(default=None, repr=False)
    log_file: Any = field(default=None, repr=False)
    timed_out: bool = False

    @property
    def label(self) -> str:
        """Return the user-facing ``tool/benchmark`` label."""

        return f"{self.tool}/{self.benchmark}"


def main(argv: list[str] | None = None) -> int:
    """Run the parallel scheduler from CLI arguments.

    Role:
    Parse user options, build one job per requested tool/benchmark pair, execute
    the scheduler, and print a final QoR table from normalized artifacts.

    Goal:
    Expose a single command for parallel sweeps with sensible local defaults and
    an explicit 10 minute per-job timeout.
    """

    args = parse_args(argv)
    jobs = build_jobs(args)
    if not jobs:
        print("No jobs selected.")
        return 1
    args.run_root.mkdir(parents=True, exist_ok=True)
    write_job_manifest(args.run_root, jobs)
    exit_code = run_jobs(jobs, args)
    txt_table, html_table = write_qor_tables(args.run_root, jobs)
    print_qor_table(txt_table)
    print(f"\nrun_root: {args.run_root}")
    print(f"status: {args.run_root / 'status.json'}")
    print(f"table: {args.run_root / 'Table.txt'}")
    print(f"html: {args.run_root / 'Table.html'}")
    return exit_code


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    """Parse options for the parallel benchmark scheduler.

    Role:
    Define controls for benchmark/tool selection, concurrency, timeout, clock,
    and local executable paths.

    Goal:
    Keep the default command useful for the current comparison while allowing
    focused reruns without editing the scheduler.
    """

    parser = argparse.ArgumentParser(description="Run HLS benchmark tools in parallel.", allow_abbrev=False)
    parser.add_argument("--benchmarks", nargs="+", default=DEFAULT_BENCHMARKS, help="benchmark names to run")
    parser.add_argument("--tools", nargs="+", choices=SUPPORTED_TOOLS, default=DEFAULT_TOOLS, help="tools to run")
    parser.add_argument("--jobs", type=int, default=max(1, min(4, os.cpu_count() or 1)), help="maximum concurrent jobs")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS, help="per-job timeout in seconds")
    parser.add_argument("--poll-interval", type=float, default=5.0, help="progress polling interval in seconds")
    parser.add_argument("--Tclk", type=float, default=DEFAULT_CLOCK_PERIOD_NS, help="target clock period in ns")
    parser.add_argument("--run-root", type=Path, default=paths.RUNS_ROOT, help="directory for parallel logs/status")
    parser.add_argument("--vivado", default=os.environ.get("VIVADO", DEFAULT_VIVADO), help="Vivado executable")
    parser.add_argument("--bambu", default=os.environ.get("BAMBU", DEFAULT_BAMBU), help="Bambu executable")
    parser.add_argument("--bambu-device", default=DEFAULT_BAMBU_DEVICE, help="Bambu device tuple")
    parser.add_argument("--dynamatic-part", default=DEFAULT_DYNAMATIC_PART, help="Dynamatic Vivado part")
    parser.add_argument("--vitis-hls", default=os.environ.get("VITIS_HLS", DEFAULT_VITIS_HLS), help="Vitis HLS executable")
    parser.add_argument("--vitis-part", default=DEFAULT_VITIS_PART, help="Vitis HLS target part")
    parser.add_argument("--vhls-cli", default=os.environ.get("VHLS_CLI", DEFAULT_VHLS_CLI), help="OpenHLS/VibeHLS CLI executable")
    parser.add_argument("--openhls-part", default=DEFAULT_OPENHLS_PART, help="OpenHLS/Vivado target part")
    parser.add_argument("--openhls-llvm-bin", default=os.environ.get("VIBEHLS_LLVM_BIN", DEFAULT_OPENHLS_LLVM_BIN), help="LLVM bin directory used by OpenHLS/VibeHLS")
    parser.add_argument("--openhls-circt-bin", default=os.environ.get("VIBEHLS_CIRCT_BIN", DEFAULT_OPENHLS_CIRCT_BIN), help="CIRCT bin directory used by OpenHLS/VibeHLS")
    parser.add_argument("--openhls-no-dse", action="store_true", help="disable automatic OpenHLS typed DSE directives")
    parser.add_argument(
        "--dynamatic-fp-units-generator",
        choices=["flopoco", "vivado"],
        default=DEFAULT_DYNAMATIC_FP_UNITS_GENERATOR,
        help="Dynamatic floating-point implementation; vivado enables Verilog FPU simulation support",
    )
    parser.add_argument(
        "--dynamatic-simulator",
        choices=["verilator", "xsim", "ghdl", "vsim"],
        default=DEFAULT_DYNAMATIC_SIMULATOR,
        help="Dynamatic simulator for exact RTL simulation; xsim supports Vivado floating-point IP",
    )
    parser.add_argument("--no-simulation", action="store_true", help="disable exact RTL simulation stages")
    parser.add_argument("--no-synthesis", action="store_true", help="disable Vivado/post-synthesis stages")
    return parser.parse_args(argv)


def build_jobs(args: argparse.Namespace) -> list[ParallelJob]:
    """Create scheduler jobs with separate report paths for every pair.

    Role:
    Translate generic tool/benchmark selections into concrete runner commands and
    unique normalized QoR paths.

    Goal:
    Avoid file races between concurrent jobs and make partial results easy to
    aggregate after a timeout or failure.
    """

    timestamp = time.strftime("%Y%m%d_%H%M%S")
    run_dir = args.run_root / timestamp
    args.run_root = run_dir
    jobs: list[ParallelJob] = []
    for benchmark in args.benchmarks:
        if "legup" in args.tools:
            jobs.append(build_legup_job(args, benchmark, run_dir))
        if "bambu" in args.tools:
            jobs.append(build_bambu_job(args, benchmark, run_dir))
        if "dynamatic" in args.tools:
            jobs.append(build_dynamatic_job(args, benchmark, run_dir))
        if "vitis" in args.tools:
            jobs.append(build_vitis_job(args, benchmark, run_dir))
        if "openhls" in args.tools:
            jobs.append(build_openhls_job(args, benchmark, run_dir))
    return jobs


def build_legup_job(args: argparse.Namespace, benchmark: str, run_dir: Path) -> ParallelJob:
    """Build one LegUp subprocess job for a benchmark."""

    qor = paths.LEGUP_WORK_ROOT / f"legup_qor_{benchmark}.json"
    command = [
        sys.executable,
        str(REPO_ROOT / "hls_bench" / "legup_runner.py"),
        "--bench",
        benchmark,
        "--period",
        f"{args.Tclk:g}",
        "--output",
        str(paths.LEGUP_WORK_ROOT / f"legup_run_{benchmark}.json"),
        "--normalized-output",
        str(qor),
        "--vivado",
        args.vivado,
    ]
    if not args.no_synthesis:
        command.append("--run-vivado")
    if not args.no_simulation:
        command.append("--run-simulation")
    if benchmark_source_is_cxx(benchmark):
        command.append("--include-cxx")
    return ParallelJob("legup", benchmark, command, run_dir / f"legup_{benchmark}.log", qor)


def benchmark_source_is_cxx(benchmark: str) -> bool:
    """Return whether the benchmark's primary source is C++.

    Role:
    Inspect the selected benchmark directory before constructing the LegUp child
    command.

    Goal:
    Pass ``--include-cxx`` only when a requested single-benchmark LegUp run needs
    it, preserving the default C-only behavior for ordinary C kernels.
    """

    source_root = paths.BENCHMARKS_ROOT / benchmark / "src"
    return any(source_root.glob(pattern) for pattern in ("*.cpp", "*.cc", "*.cxx"))


def build_bambu_job(args: argparse.Namespace, benchmark: str, run_dir: Path) -> ParallelJob:
    """Build one Bambu/PANDA subprocess job for a benchmark."""

    qor = paths.BAMBU_WORK_ROOT / f"bambu_qor_{benchmark}.json"
    report = paths.BAMBU_WORK_ROOT / f"bambu_report_{benchmark}.json"
    command = [
        sys.executable,
        str(CLI),
        "bambu",
        "--",
        "--benchmarks",
        benchmark,
        "--bambu",
        args.bambu,
        "--vivado",
        args.vivado,
        "--device-name",
        args.bambu_device,
        "--clock-period",
        f"{args.Tclk:g}",
        "--timeout",
        f"{args.timeout:g}",
        "--summary",
        str(report),
        "--normalized-summary",
        str(qor),
    ]
    if not args.no_simulation:
        command.append("--run-simulation")
    if not args.no_synthesis:
        command.append("--run-synthesis")
    return ParallelJob("bambu", benchmark, command, run_dir / f"bambu_{benchmark}.log", qor)


def build_dynamatic_job(args: argparse.Namespace, benchmark: str, run_dir: Path) -> ParallelJob:
    """Build one Dynamatic subprocess job for a benchmark."""

    qor = paths.DYNAMATIC_WORK_ROOT / f"dynamatic_qor_{benchmark}.json"
    report = paths.DYNAMATIC_WORK_ROOT / f"dynamatic_run_{benchmark}.json"
    command = [
        sys.executable,
        str(CLI),
        "dynamatic",
        "--bench",
        benchmark,
        "--Tclk",
        f"{args.Tclk:g}",
        "--timeout",
        f"{args.timeout:g}",
        "--part",
        args.dynamatic_part,
        "--fp-units-generator",
        args.dynamatic_fp_units_generator,
        "--simulator",
        args.dynamatic_simulator,
        "--summary",
        str(report),
        "--normalized-summary",
        str(qor),
    ]
    if not args.no_simulation:
        command.append("--run-simulation")
    if not args.no_synthesis:
        command.append("--run-synthesis")
    return ParallelJob("dynamatic", benchmark, command, run_dir / f"dynamatic_{benchmark}.log", qor)


def build_vitis_job(args: argparse.Namespace, benchmark: str, run_dir: Path) -> ParallelJob:
    """Build one Vitis HLS subprocess job for a benchmark.

    Role:
    Create an isolated command line and normalized QoR path for one Vitis HLS
    benchmark run.

    Goal:
    Let Vitis participate in the same parallel scheduler as the other tools while
    avoiding report-file races across benchmarks.
    """

    qor = paths.VITIS_WORK_ROOT / f"vitis_qor_{benchmark}.json"
    report = paths.VITIS_WORK_ROOT / f"vitis_run_{benchmark}.json"
    command = [
        sys.executable,
        str(CLI),
        "vitis",
        "--bench",
        benchmark,
        "--Tclk",
        f"{args.Tclk:g}",
        "--timeout",
        f"{args.timeout:g}",
        "--vitis-hls",
        args.vitis_hls,
        "--vivado",
        args.vivado,
        "--part",
        args.vitis_part,
        "--summary",
        str(report),
        "--normalized-summary",
        str(qor),
    ]
    if not args.no_synthesis:
        command.append("--run-synthesis")
    if not args.no_simulation:
        command.append("--run-simulation")
    return ParallelJob("vitis", benchmark, command, run_dir / f"vitis_{benchmark}.log", qor)


def build_openhls_job(args: argparse.Namespace, benchmark: str, run_dir: Path) -> ParallelJob:
    """Build one OpenHLS/VibeHLS subprocess job for a benchmark.

    Role:
    Create an isolated command line and normalized QoR path for one staged
    OpenHLS benchmark run.

    Goal:
    Let OpenHLS participate in the same parallel scheduler and shared table as the
    other HLS tools while respecting the scheduler's simulation/synthesis toggles.
    """

    qor = paths.OPENHLS_WORK_ROOT / f"openhls_qor_{benchmark}.json"
    report = paths.OPENHLS_WORK_ROOT / f"openhls_run_{benchmark}.json"
    command = [
        sys.executable,
        str(CLI),
        "openhls",
        "--bench",
        benchmark,
        "--Tclk",
        f"{args.Tclk:g}",
        "--timeout",
        f"{args.timeout:g}",
        "--vhls-cli",
        args.vhls_cli,
        "--vivado",
        args.vivado,
        "--llvm-bin",
        args.openhls_llvm_bin,
        "--circt-bin",
        args.openhls_circt_bin,
        "--part",
        args.openhls_part,
        "--summary",
        str(report),
        "--normalized-summary",
        str(qor),
    ]
    if not args.no_simulation:
        command.append("--run-simulation")
    if not args.no_synthesis:
        command.append("--run-synthesis")
    if args.openhls_no_dse:
        command.append("--no-dse")
    return ParallelJob("openhls", benchmark, command, run_dir / f"openhls_{benchmark}.log", qor)


def run_jobs(jobs: list[ParallelJob], args: argparse.Namespace) -> int:
    """Execute jobs with bounded concurrency and per-job timeout.

    Role:
    Start pending jobs until the concurrency limit is reached, poll running jobs,
    terminate timed-out process groups, and persist status after every change.

    Goal:
    Provide visible progress and prevent one stalled tool invocation from blocking
    the full sweep indefinitely.
    """

    started = time.time()
    print(f"parallel sweep: {len(jobs)} jobs, concurrency={args.jobs}, timeout={args.timeout:g}s")
    while True:
        changed = finish_completed_jobs(jobs)
        changed = kill_timed_out_jobs(jobs, args.timeout) or changed
        changed = start_pending_jobs(jobs, args.jobs) or changed
        if changed:
            write_status(args.run_root, jobs, started)
        if all(job.status in {"ok", "failed", "timeout"} for job in jobs):
            break
        time.sleep(args.poll_interval)
    write_status(args.run_root, jobs, started)
    failures = [job for job in jobs if job.status != "ok"]
    print(f"completed {len(jobs) - len(failures)}/{len(jobs)} jobs")
    return 1 if failures else 0


def start_pending_jobs(jobs: list[ParallelJob], max_jobs: int) -> bool:
    """Start pending subprocesses until the concurrency limit is full."""

    changed = False
    while count_running(jobs) < max_jobs:
        job = next((item for item in jobs if item.status == "pending"), None)
        if job is None:
            break
        job.log_path.parent.mkdir(parents=True, exist_ok=True)
        job.log_file = job.log_path.open("w", encoding="utf-8")
        job.log_file.write("# command: " + shlex.join(job.command) + "\n\n")
        job.log_file.flush()
        job.process = subprocess.Popen(
            job.command,
            cwd=REPO_ROOT,
            stdout=job.log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        job.status = "running"
        job.started_at = time.time()
        changed = True
        print(f"[{time.strftime('%H:%M:%S')}] started {job.label} pid={job.process.pid}", flush=True)
    return changed


def finish_completed_jobs(jobs: list[ParallelJob]) -> bool:
    """Collect return codes for subprocesses that exited naturally."""

    changed = False
    for job in jobs:
        if job.status != "running" or job.process is None:
            continue
        returncode = job.process.poll()
        if returncode is None:
            continue
        job.returncode = returncode
        job.ended_at = time.time()
        job.status = "ok" if returncode == 0 else "failed"
        close_job_log(job)
        changed = True
        print(f"[{time.strftime('%H:%M:%S')}] finished {job.label} rc={returncode}", flush=True)
    return changed


def kill_timed_out_jobs(jobs: list[ParallelJob], timeout_seconds: float) -> bool:
    """Terminate process groups for jobs that exceeded the timeout."""

    changed = False
    now = time.time()
    for job in jobs:
        if job.status != "running" or job.process is None or job.started_at is None:
            continue
        if now - job.started_at <= timeout_seconds:
            continue
        try:
            os.killpg(job.process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            job.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(job.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            job.process.wait()
        job.returncode = job.process.returncode
        job.ended_at = time.time()
        job.status = "timeout"
        job.timed_out = True
        close_job_log(job)
        changed = True
        print(f"[{time.strftime('%H:%M:%S')}] timeout {job.label} after {timeout_seconds:g}s", flush=True)
    return changed


def close_job_log(job: ParallelJob) -> None:
    """Close a job log file handle if it is still open."""

    if job.log_file is not None:
        job.log_file.close()
        job.log_file = None


def count_running(jobs: list[ParallelJob]) -> int:
    """Return the number of currently running jobs."""

    return sum(1 for job in jobs if job.status == "running")


def write_job_manifest(run_root: Path, jobs: list[ParallelJob]) -> None:
    """Write the static command manifest for the parallel sweep."""

    run_root.mkdir(parents=True, exist_ok=True)
    payload = [job_status(job, include_command=True) for job in jobs]
    (run_root / "tasks.json").write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_status(run_root: Path, jobs: list[ParallelJob], started: float) -> None:
    """Write a machine-readable live status snapshot."""

    counts: dict[str, int] = {}
    for job in jobs:
        counts[job.status] = counts.get(job.status, 0) + 1
    payload = {
        "elapsed_seconds": round(time.time() - started, 1),
        "counts": counts,
        "tasks": [job_status(job, include_command=False) for job in jobs],
    }
    (run_root / "status.json").write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def job_status(job: ParallelJob, *, include_command: bool) -> dict[str, Any]:
    """Serialize one job state for manifest or live status files."""

    payload: dict[str, Any] = {
        "tool": job.tool,
        "benchmark": job.benchmark,
        "label": job.label,
        "status": job.status,
        "returncode": job.returncode,
        "started_at": job.started_at,
        "ended_at": job.ended_at,
        "timed_out": job.timed_out,
        "log": str(job.log_path),
        "qor": str(job.qor_path),
    }
    if include_command:
        payload["command"] = job.command
    return payload


def print_qor_table(table_text: str) -> None:
    """Print an ASCII QoR table from normalized per-job JSON reports.

    Role:
    Read every completed job's normalized QoR artifact and extract status,
    execution cycles, post-synthesis resources, and timing.

    Goal:
    Give a single comparison table while still making failed or timed-out jobs
    visible with their log paths.
    """

    print(table_text)


def write_qor_tables(run_root: Path, jobs: list[ParallelJob]) -> tuple[str, str]:
    """Render and persist the parallel QoR summary tables.

    Role:
    Convert the normalized per-job QoR records into both a fixed-width ASCII table
    and a small HTML table.

    Goal:
    Produce human-readable artifacts alongside the console summary so the sweep can
    be shared or archived without scraping terminal output.
    """

    rows = [qor_row(job) for job in jobs]
    headers = ["kernel", "tool", "status", "cycles", "Tclk_ns", "II", "exec_ns", "LUT", "FF", "BRAM", "DSP", "res_avg_%", "slack_ns", "log"]
    txt_table = render_ascii_table(headers, rows, group_key="kernel")
    reports_root = paths.REPORTS_ROOT
    reports_root.mkdir(parents=True, exist_ok=True)
    run_figure_dir = run_root / "figures"
    reports_figure_dir = reports_root / "figures"
    plot_qor_figures(rows, run_figure_dir)
    plot_qor_figures(rows, reports_figure_dir)
    html_table = render_html_table(headers, rows, group_key="kernel", figure_dir=run_figure_dir, figure_href_prefix="figures")
    reports_html_table = render_html_table(headers, rows, group_key="kernel", figure_dir=reports_figure_dir, figure_href_prefix="figures")
    (run_root / "Table.txt").write_text(txt_table + "\n", encoding="utf-8")
    (run_root / "Table.html").write_text(html_table, encoding="utf-8")
    (reports_root / "Table.txt").write_text(txt_table + "\n", encoding="utf-8")
    (reports_root / "Table.html").write_text(reports_html_table, encoding="utf-8")
    loop_txt, loop_html = write_loop_ii_reports(run_root, jobs)
    (reports_root / "LoopII.txt").write_text(loop_txt + "\n", encoding="utf-8")
    (reports_root / "LoopII.html").write_text(loop_html, encoding="utf-8")
    link_loop_report(run_root / "Table.html")
    link_loop_report(reports_root / "Table.html")
    return txt_table, html_table


def link_loop_report(table_html: Path) -> None:
    """Add a Loop II report link to one generated HTML table.

    Role:
    Keep the main QoR report connected to the separate loop-initiation-interval
    comparison without mixing loop rows into the already dense resource table.

    Goal:
    Make regenerated reports self-navigable while preserving the standalone
    ``LoopII.html`` artifact for direct sharing.
    """

    if not table_html.is_file():
        return
    text = table_html.read_text(encoding="utf-8")
    link = '<a href="LoopII.html">loop II comparison</a>'
    if link in text:
        return
    text = text.replace("<strong>Figures:</strong>", "<strong>Reports:</strong>\n" + link + "\n<strong>Figures:</strong>", 1)
    table_html.write_text(text, encoding="utf-8")


def render_ascii_table(headers: list[str], rows: list[dict[str, str]], *, group_key: str | None = None, title: str = "QoR summary") -> str:
    """Render one fixed-width ASCII table from normalized QoR rows.

    Role:
    Produce the text report used in terminals and archived ``Table.txt`` files.

    Goal:
    Keep kernel grouping readable even though plain text cannot merge cells; when
    ``group_key`` is set, repeated group values are printed only on the first row
    of each group.
    """

    display_rows = grouped_display_rows(headers, rows, group_key)
    widths = [max(len(header), *(len(row.get(header, "")) for row in display_rows)) for header in headers]
    lines = [title, " | ".join(headers[index].ljust(widths[index]) for index in range(len(headers))), "-+-".join("-" * width for width in widths)]
    for row in display_rows:
        lines.append(" | ".join(row.get(header, "").ljust(widths[index]) for index, header in enumerate(headers)))
    return "\n".join(lines)


def render_html_table(headers: list[str], rows: list[dict[str, str]], *, group_key: str | None = None, figure_dir: Path | None = None, figure_href_prefix: str = "figures") -> str:
    """Render one HTML QoR table with grouped kernel cells and plot links.

    Role:
    Create the browser-facing report from the same rows as the text table.

    Goal:
    Merge repeated kernel names with ``rowspan`` so each benchmark block is easy
    to scan, and expose generated Pareto figures next to the numerical table.
    """

    lines = ["<!doctype html>", "<html>", "<head>", '<meta charset="utf-8">', "<title>QoR summary</title>", "<style>", "body { font-family: system-ui, sans-serif; }", "table { border-collapse: collapse; font-family: monospace; }", "th, td { border: 1px solid #999; padding: 4px 8px; text-align: left; }", "th { background: #eee; position: sticky; top: 0; }", "td.group { font-weight: 700; background: #f7f7f7; vertical-align: top; }", ".figures { margin: 1rem 0; }", ".figures a { margin-right: 1rem; }", "</style>", "</head>", "<body>", "<h1>QoR summary</h1>"]
    if figure_dir is not None:
        lines.extend(render_figure_links(rows, figure_dir, figure_href_prefix))
    lines.extend(["<table>", "<thead>", "<tr>"])
    for header in headers:
        lines.append(f"<th>{html.escape(header)}</th>")
    lines.extend(["</tr>", "</thead>", "<tbody>"])
    group_spans = row_group_spans(rows, group_key)
    for index, row in enumerate(rows):
        lines.append("<tr>")
        for header in headers:
            if header == group_key:
                span = group_spans.get(index)
                if span is None:
                    continue
                lines.append(f'<td class="group" rowspan="{span}">{html.escape(row.get(header, ""))}</td>')
                continue
            value = row.get(header, "")
            lines.append(f"<td>{html.escape(value)}</td>")
        lines.append("</tr>")
    lines.extend(["</tbody>", "</table>", "</body>", "</html>"])
    return "\n".join(lines)


def grouped_display_rows(headers: list[str], rows: list[dict[str, str]], group_key: str | None) -> list[dict[str, str]]:
    """Return rows with repeated text group labels blanked for ASCII output."""

    if group_key is None:
        return rows
    out: list[dict[str, str]] = []
    previous: str | None = None
    for row in rows:
        current = row.get(group_key, "")
        display = {header: row.get(header, "") for header in headers}
        if current == previous:
            display[group_key] = ""
        previous = current
        out.append(display)
    return out


def row_group_spans(rows: list[dict[str, str]], group_key: str | None) -> dict[int, int]:
    """Return first-row indexes and span lengths for repeated HTML group cells."""

    if group_key is None:
        return {}
    spans: dict[int, int] = {}
    index = 0
    while index < len(rows):
        value = rows[index].get(group_key, "")
        end = index + 1
        while end < len(rows) and rows[end].get(group_key, "") == value:
            end += 1
        spans[index] = end - index
        index = end
    return spans


def qor_row(job: ParallelJob) -> dict[str, str]:
    """Build one table row from a job and its normalized QoR JSON."""

    row = {
        "kernel": job.benchmark,
        "tool": job.tool,
        "status": job.status,
        "cycles": "-",
        "Tclk_ns": "-",
        "II": "-",
        "exec_ns": "-",
        "LUT": "-",
        "FF": "-",
        "BRAM": "-",
        "DSP": "-",
        "res_avg_%": "-",
        "slack_ns": "-",
        "log": str(job.log_path),
        "_cycles": None,
        "_tclk_ns": None,
        "_exec_ns": None,
        "_res_avg_pct": None,
    }
    record = load_single_qor_record(job.qor_path, job.benchmark)
    if record is None:
        return row
    row["status"] = str(record.get("status") or job.status)
    latency = record.get("latency") or {}
    post = record.get("post_synth") or {}
    estimate = record.get("tool_estimate") or {}
    if all(post.get(key) is None for key in ("lut", "ff", "bram", "dsp", "tclk_ns", "slack_ns")):
        post = estimate
    cycles = to_float(latency.get("cycles"))
    tclk_ns = to_float(post.get("tclk_ns"))
    exec_ns = cycles * tclk_ns if cycles is not None and tclk_ns is not None else None
    res_avg_pct = resource_average_percent(post)
    row["_cycles"] = cycles
    row["_tclk_ns"] = tclk_ns
    row["_exec_ns"] = exec_ns
    row["_res_avg_pct"] = res_avg_pct
    row["cycles"] = format_cell(latency.get("cycles"))
    row["Tclk_ns"] = format_cell(post.get("tclk_ns"))
    row["II"] = pipeline_ii_cell(job)
    row["exec_ns"] = format_cell(exec_ns)
    row["LUT"] = format_cell(post.get("lut"))
    row["FF"] = format_cell(post.get("ff"))
    row["BRAM"] = format_cell(post.get("bram"))
    row["DSP"] = format_cell(post.get("dsp"))
    row["res_avg_%"] = format_cell(res_avg_pct)
    row["slack_ns"] = format_cell(post.get("slack_ns"))
    return row


def pipeline_ii_cell(job: ParallelJob) -> str:
    """Return reported loop IIs in the kernel's textual source order.

    Role:
    Reuse the per-tool report parsers used by the Loop II report and flatten a
    kernel's possibly multiple pipeline IIs into one QoR-table cell.

    Goal:
    Show only tool-reported initiation intervals as ``II/II/II`` beside timing,
    without inventing an aggregate value or changing the standalone loop report.
    """

    parsers = {
        "legup": parse_legup_loop_ii,
        "vitis": parse_vitis_loop_ii,
        "openhls": parse_openhls_loop_ii,
    }
    parser = parsers.get(job.tool)
    if parser is None:
        return "-"
    loops = parser(job.benchmark)
    if not loops:
        return "-"
    order = source_loop_order(job.benchmark)
    ordered_loops = sorted(loops.items(), key=lambda item: (order.get(item[0], len(order)), item[0]))
    return "/".join(format_cell(loop.get("ii")) for _, loop in ordered_loops)


def source_loop_order(kernel: str) -> dict[str, int]:
    """Map canonical source loop labels to their textual order for one kernel.

    Role:
    Read the benchmark's project metadata and apply the shared loop-label
    discovery routine without modifying the source tree.

    Goal:
    Keep the compact QoR-table II sequence aligned with source order even when
    individual tools emit loop results in scheduler or report-file order.
    """

    source_dir = paths.BENCHMARKS_ROOT / kernel / "src"
    project_path = source_dir / "ohls_project.json"
    try:
        project = json.loads(project_path.read_text(encoding="utf-8"))
        source = source_dir / str(project["kernel"])
        entries = loop_labels.label_c_source_text(source.read_text(encoding="utf-8", errors="replace"))[1]
    except (KeyError, OSError, ValueError, TypeError):
        return {}
    return {entry.label: index for index, entry in enumerate(entries)}


def resource_average_percent(post: dict[str, Any]) -> float | None:
    """Return average Artix-7 A100T utilization percentage for one QoR record.

    Role:
    Collapse LUT, FF, BRAM, and DSP usage into a single approximate footprint
    score that can be plotted against execution time.

    Goal:
    Compare tools with one resource-pressure axis while keeping missing values
    honest: only available dimensions are averaged, and no value is reported when
    all device resource dimensions are absent.
    """

    percentages: list[float] = []
    for key, capacity in ARTIX7_A100T_CAPACITY.items():
        value = to_float(post.get(key))
        if value is not None:
            percentages.append((value / capacity) * 100.0)
    if not percentages:
        return None
    return sum(percentages) / len(percentages)


def to_float(value: Any) -> float | None:
    """Convert normalized QoR scalar values to float or return ``None``."""

    if value is None or value == "-":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def plot_qor_figures(rows: list[dict[str, str]], figure_dir: Path) -> None:
    """Generate per-kernel and global QoR scatter plots with matplotlib.

    Role:
    Visualize the same derived metrics as the table: exact execution time in ns
    and average Artix-7 resource utilization percentage.

    Goal:
    Make Pareto tradeoffs easy to inspect per kernel and provide one normalized
    global scatter where tool colors remain consistent across all benchmarks.
    Plot generation is best-effort so missing matplotlib does not break table
    generation on minimal environments.
    """

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return

    figure_dir.mkdir(parents=True, exist_ok=True)
    plot_rows = [row for row in rows if row.get("_exec_ns") is not None and row.get("_res_avg_pct") is not None]
    kernels = sorted({row["kernel"] for row in plot_rows})
    for kernel in kernels:
        kernel_rows = [row for row in plot_rows if row["kernel"] == kernel]
        if not kernel_rows:
            continue
        fig, ax = plt.subplots(figsize=(6, 4))
        for row in kernel_rows:
            color = TOOL_COLORS.get(row["tool"], "#333333")
            ax.scatter(row["_exec_ns"], row["_res_avg_pct"], color=color, s=55, label=row["tool"])
            ax.annotate(row["tool"], (row["_exec_ns"], row["_res_avg_pct"]), textcoords="offset points", xytext=(5, 4), fontsize=8)
        ax.set_title(f"{kernel}: execution time vs resource footprint")
        ax.set_xlabel("exec_ns = cycles * post-synth Tclk")
        ax.set_ylabel("average A7-100T resource use (%)")
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(figure_dir / f"{kernel}_pareto.png", dpi=160)
        plt.close(fig)

    global_rows = normalized_global_rows(plot_rows)
    if global_rows:
        fig, ax = plt.subplots(figsize=(8, 5))
        for tool in SUPPORTED_TOOLS:
            tool_rows = [row for row in global_rows if row["tool"] == tool]
            if not tool_rows:
                continue
            ax.scatter([row["exec_norm"] for row in tool_rows], [row["area_norm"] for row in tool_rows], color=TOOL_COLORS.get(tool, "#333333"), s=50, label=tool, alpha=0.85)
        ax.set_title("Global normalized QoR scatter")
        ax.set_xlabel("exec_ns / best exec_ns for kernel")
        ax.set_ylabel("res_avg_% / best res_avg_% for kernel")
        ax.grid(True, alpha=0.3)
        ax.legend()
        fig.tight_layout()
        fig.savefig(figure_dir / "global_normalized_qor.png", dpi=180)
        plt.close(fig)


def normalized_global_rows(rows: list[dict[str, str]]) -> list[dict[str, Any]]:
    """Return per-row normalized execution and area values for global plotting."""

    out: list[dict[str, Any]] = []
    kernels = sorted({row["kernel"] for row in rows})
    for kernel in kernels:
        kernel_rows = [row for row in rows if row["kernel"] == kernel]
        best_exec = min((row["_exec_ns"] for row in kernel_rows if row.get("_exec_ns") and row["_exec_ns"] > 0), default=None)
        best_area = min((row["_res_avg_pct"] for row in kernel_rows if row.get("_res_avg_pct") and row["_res_avg_pct"] > 0), default=None)
        if best_exec is None or best_area is None:
            continue
        for row in kernel_rows:
            exec_ns = row.get("_exec_ns")
            area = row.get("_res_avg_pct")
            if exec_ns is None or area is None:
                continue
            out.append({"kernel": kernel, "tool": row["tool"], "exec_norm": exec_ns / best_exec, "area_norm": area / best_area})
    return out


def render_figure_links(rows: list[dict[str, str]], figure_dir: Path, href_prefix: str) -> list[str]:
    """Render links to generated Pareto figures for the HTML report."""

    links = ["<div class=\"figures\">", "<strong>Figures:</strong>"]
    global_path = figure_dir / "global_normalized_qor.png"
    if global_path.is_file():
        links.append(f'<a href="{html.escape(href_prefix)}/global_normalized_qor.png">global normalized QoR</a>')
    for kernel in sorted({row["kernel"] for row in rows}):
        path = figure_dir / f"{kernel}_pareto.png"
        if path.is_file():
            links.append(f'<a href="{html.escape(href_prefix)}/{html.escape(kernel)}_pareto.png">{html.escape(kernel)}</a>')
    links.append("</div>")
    return links


def write_loop_ii_reports(run_root: Path, jobs: list[ParallelJob]) -> tuple[str, str]:
    """Write loop initiation interval comparison reports for selected kernels.

    Role:
    Gather effective loop II data emitted by LegUp, Vitis HLS, and OpenHLS from
    their existing report artifacts and render a compact cross-tool comparison.

    Goal:
    Compare per-loop pipeline quality without inventing loop matches: labels are
    normalized conservatively, and tool-specific unmatched loops remain visible as
    separate rows instead of being silently merged with unrelated loops.
    """

    kernels = []
    for job in jobs:
        if job.benchmark not in kernels:
            kernels.append(job.benchmark)
    tables = build_loop_ii_tables(kernels)
    txt = render_loop_ii_text(tables)
    html_text = render_loop_ii_html(tables)
    (run_root / "LoopII.txt").write_text(txt + "\n", encoding="utf-8")
    (run_root / "LoopII.html").write_text(html_text, encoding="utf-8")
    return txt, html_text


def build_loop_ii_tables(kernels: list[str]) -> list[dict[str, Any]]:
    """Build one tool-by-loop metric table for each selected kernel.

    A kernel owns its loop namespace, so representing each kernel as a separate
    table avoids false matches between identically named loops in other kernels.
    Each tool remains a row, while every normalized loop owns adjacent II and
    depth columns.  OpenHLS reports latency instead of pipeline depth; that value
    occupies the common second metric column and is identified in the report note.
    """

    tables: list[dict[str, Any]] = []
    for kernel in kernels:
        per_tool = {
            "legup": parse_legup_loop_ii(kernel),
            "bambu": {},
            "dynamatic": {},
            "vitis": parse_vitis_loop_ii(kernel),
            "openhls": parse_openhls_loop_ii(kernel),
        }
        loop_keys: list[str] = []
        for loops in per_tool.values():
            for key in loops:
                if key not in loop_keys:
                    loop_keys.append(key)
        reported_loops = [loops for loops in per_tool.values() if loops]
        if reported_loops and all(len(loops) == 1 for loops in reported_loops):
            # A tool may rename the only source loop (for example, LegUp's
            # generated state suffix).  With no competing loop in that kernel,
            # the metrics describe the same loop and must share one column.
            source_labels = sorted(key for key in loop_keys if not key.startswith("nolabel"))
            loop_keys = [source_labels[0] if source_labels else loop_keys[0]]
            logical_loop = loop_keys[0]
            per_tool = {
                tool: {logical_loop: next(iter(loops.values()))} if loops else {}
                for tool, loops in per_tool.items()
            }
        tables.append({"kernel": kernel, "loops": sorted(loop_keys) or ["-"], "tools": per_tool})
    return tables


def parse_legup_loop_ii(kernel: str) -> dict[str, dict[str, Any]]:
    """Parse LegUp loop II records from normalized run JSON.

    Role:
    Reuse the loop parser already run by the LegUp flow instead of re-parsing
    report text in the aggregate reporter.

    Goal:
    Surface LegUp's reported ``II`` and depth when LegUp actually generated loop
    pipeline data; many current kernels have empty LegUp pipeline reports and are
    therefore reported as missing rather than guessed.
    """

    path = paths.LEGUP_WORK_ROOT / f"legup_run_{kernel}.json"
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    record = (payload.get("benchmarks") or {}).get(kernel) or {}
    loops = record.get("pipelined_loops") or (record.get("schedule") or {}).get("loops") or []
    out: dict[str, dict[str, Any]] = {}
    for loop in loops:
        label = str(loop.get("label") or loop.get("bb") or "loop")
        key = normalize_loop_name(label, kernel)
        out[key] = {"ii": loop.get("ii"), "depth": loop.get("depth_cycles"), "source": label}
    return out


def parse_vitis_loop_ii(kernel: str) -> dict[str, dict[str, Any]]:
    """Parse Vitis HLS loop metrics from top-level and pipeline csynth reports.

    Role:
    Read the top-level synthesis report first, then supplement loops that omit
    pipeline metrics with their dedicated Vitis pipeline-submodule reports.

    Goal:
    Keep the top-level report authoritative when it provides an II while exposing
    pipeline IIs that Vitis reports only in ``*_Pipeline_*_csynth.xml`` files,
    such as ``qr_givens``'s ``L_2_2`` II=14.

    How:
    Insert top-level report records before submodule records and retain the first
    value per normalized loop name.  Design-size XML remains excluded because it
    contains no source-loop performance data.
    """

    report_dir = paths.VITIS_WORK_ROOT / kernel / "project" / "solution1" / "syn" / "report"
    out: dict[str, dict[str, Any]] = {}
    if not report_dir.is_dir():
        return out
    primary = report_dir / f"{kernel}_csynth.xml"
    pipeline_reports = sorted(report_dir.glob("*_Pipeline_*_csynth.xml"))
    if primary.is_file():
        candidates = [primary, *pipeline_reports]
    else:
        candidates = sorted(report_dir.glob("*_csynth.xml"))
    for path in candidates:
        if path.name == "csynth_design_size.xml":
            continue
        try:
            root = ET.parse(path).getroot()
        except Exception:
            continue
        summary = root.find(".//SummaryOfLoopLatency")
        if summary is None:
            continue
        for child in list(summary):
            ii = text_to_int(child.findtext("PipelineII"))
            depth = text_to_int(child.findtext("PipelineDepth"))
            source = child.tag
            key = normalize_loop_name(source, kernel)
            if ii is None:
                continue
            out.setdefault(key, {"ii": ii, "depth": depth, "source": source})
    return out


def parse_openhls_loop_ii(kernel: str) -> dict[str, dict[str, Any]]:
    """Parse OpenHLS pipeline loop II rows from schedule.report.txt."""

    schedule_dir = paths.OPENHLS_WORK_ROOT / kernel / "output" / "scheduling"
    candidates = sorted(schedule_dir.glob("*.schedule.report.txt")) if schedule_dir.is_dir() else []
    out: dict[str, dict[str, Any]] = {}
    for path in candidates:
        in_table = False
        for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if raw.strip() == "Pipeline Loops":
                in_table = True
                continue
            if not in_table or "|" not in raw:
                continue
            cells = [cell.strip() for cell in raw.strip().strip("|").split("|")]
            if not cells or cells[0] in {"loop", ""} or set(cells[0]) == {"-"}:
                continue
            if len(cells) < 10:
                if out:
                    break
                continue
            name = cells[0]
            ii = text_to_int(cells[8])
            latency = text_to_int(cells[9])
            if ii is None:
                continue
            key = normalize_loop_name(name, kernel)
            out[key] = {"ii": ii, "latency": latency, "source": name}
    return out


def normalize_loop_name(name: str, kernel: str) -> str:
    """Conservatively normalize tool-specific loop names for table grouping."""

    out = name.strip().strip("-").strip()
    for prefix in (f"{kernel}_Pipeline_", f"{kernel}_", "Pipeline_"):
        if out.startswith(prefix):
            out = out[len(prefix):]
    out = re.sub(r"^VITIS_LOOP_", "loop_", out)
    out = re.sub(r"^vitis_pipeline_", "pipeline_", out)
    return out or name


def text_to_int(text: str | None) -> int | None:
    """Parse an integer field from a report cell or XML text."""

    if text is None:
        return None
    cleaned = text.strip().replace(",", "")
    if not cleaned or cleaned == "-":
        return None
    try:
        return int(cleaned)
    except ValueError:
        return None


def loop_metric_cells(tool: str, loop: dict[str, Any] | None) -> tuple[str, str]:
    """Return displayable II and depth/latency cells for one tool and loop."""

    if loop is None:
        return "-", "-"
    metric = "latency" if tool == "openhls" else "depth"
    return format_cell(loop.get("ii")), format_cell(loop.get(metric))


def render_loop_ii_text(tables: list[dict[str, Any]]) -> str:
    """Render the transposed Loop II tables in the plain-text report companion."""

    sections = ["Loop II comparison", "OpenHLS uses latency in the Depth column because it does not report pipeline depth. Bambu and Dynamatic do not emit a static per-source-loop II or depth."]
    for table in tables:
        headers = ["tool"]
        for loop in table["loops"]:
            headers.extend([f"{loop} II", f"{loop} Depth"])
        rows = []
        for tool in LOOP_II_TOOLS:
            row = {"tool": tool}
            for loop_name in table["loops"]:
                ii, depth = loop_metric_cells(tool, table["tools"][tool].get(loop_name))
                row[f"{loop_name} II"] = ii
                row[f"{loop_name} Depth"] = depth
            rows.append(row)
        sections.append(render_ascii_table(headers, rows, title=table["kernel"]))
    return "\n\n".join(sections)


def render_loop_ii_html(tables: list[dict[str, Any]]) -> str:
    """Render Table.html-style kernel/tool rows with grouped loop metrics."""

    lines = ["<!doctype html>", "<html>", "<head>", '<meta charset="utf-8">', "<title>Loop II comparison</title>", "<style>", "body { font-family: system-ui, sans-serif; }", "table { border-collapse: collapse; font-family: monospace; }", "th, td { border: 1px solid #999; padding: 4px 8px; text-align: left; vertical-align: top; }", "th { background: #eee; position: sticky; top: 0; }", "td.group { font-weight: 700; background: #f7f7f7; vertical-align: top; }", ".figures { margin: 1rem 0; }", ".figures a { margin-right: 1rem; }", "</style>", "</head>", "<body>", "<h1>Loop II comparison</h1>", '<div class="figures">', '<strong>Reports:</strong>', '<a href="Table.html">QoR summary</a>', "</div>", "<p>OpenHLS reports loop latency rather than pipeline depth; its latency is shown in the Depth column. Bambu and Dynamatic do not emit a static per-source-loop II or depth.</p>"]
    for table in tables:
        lines.extend(["<table>", "<thead>", "<tr>", '<th rowspan="2">kernel</th>', '<th rowspan="2">tool</th>'])
        for loop_name in table["loops"]:
            lines.append(f'<th colspan="2">{html.escape(loop_name)}</th>')
        lines.append("</tr>")
        lines.append("<tr>")
        for _ in table["loops"]:
            lines.extend(["<th>II</th>", "<th>Depth</th>"])
        lines.extend(["</tr>", "</thead>", "<tbody>"])
        for index, tool in enumerate(LOOP_II_TOOLS):
            lines.append("<tr>")
            if index == 0:
                lines.append(f'<td class="group" rowspan="{len(LOOP_II_TOOLS)}">{html.escape(table["kernel"])}</td>')
            lines.append(f'<td class="group">{html.escape(tool)}</td>')
            for loop_name in table["loops"]:
                ii, depth = loop_metric_cells(tool, table["tools"][tool].get(loop_name))
                lines.append(f"<td>{html.escape(ii)}</td>")
                lines.append(f"<td>{html.escape(depth)}</td>")
            lines.append("</tr>")
        lines.extend(["</tbody>", "</table>"])
    lines.extend(["</body>", "</html>"])
    return "\n".join(lines)


def load_single_qor_record(path: Path, benchmark: str) -> dict[str, Any] | None:
    """Load one benchmark record from a normalized QoR JSON file."""

    if not path.is_file():
        return None
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    for record in payload.get("benchmarks") or []:
        if record.get("benchmark") == benchmark:
            return record
    return None


def format_cell(value: Any) -> str:
    """Format a QoR value for the ASCII table."""

    if value is None:
        return "-"
    if isinstance(value, float):
        if abs(value) >= 1000.0:
            return f"{value:.1f}"
        return f"{value:.3f}"
    return str(value)


if __name__ == "__main__":
    raise SystemExit(main())
