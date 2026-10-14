"""Normalize HLS quality-of-results data across benchmark tools.

Role:
Provide a common JSON shape for benchmark status, latency, tool estimates, and
post-synthesis implementation metrics while preserving each runner's native
reports as separate artifacts.

Goal:
Let LegUp, Bambu/PANDA, and future Dynamatic runs be compared by downstream
scripts without scraping several tool-specific logs or replacing the current
runner outputs.

How:
The module consumes already generated files: batch-run result objects, Bambu
transcripts, LegUp report dictionaries, OpenHLS cycle-count JSON, and Vivado
utilization/timing reports.  It performs no synthesis and never substitutes a
fallback value that would make a missing metric look valid; unavailable data are
reported as ``None`` with source fields left unset.
"""

from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import re
from typing import Any
import xml.etree.ElementTree as ET


def utc_timestamp() -> str:
    """Return a stable UTC timestamp for generated common reports.

    Role:
    Centralize timestamp formatting so every normalized report produced by the
    runners uses the same timezone-aware representation.

    Goal:
    Make report artifacts comparable across tools and avoid mixing local-time
    and UTC timestamps in generated JSON files.
    """

    return datetime.now(tz=timezone.utc).isoformat()


def write_json(path: Path, payload: dict[str, Any]) -> None:
    """Write one normalized JSON artifact with deterministic formatting.

    Role:
    Create parent directories and serialize dictionaries in a stable, readable
    form for humans and regression tooling.

    Goal:
    Keep file output behavior identical for all runners that use the shared QoR
    module.
    """

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def build_report(*, tool: str, config: dict[str, Any], benchmarks: list[dict[str, Any]]) -> dict[str, Any]:
    """Build the top-level normalized QoR report document.

    Role:
    Wrap normalized benchmark records with schema, generation time, tool name,
    run configuration, and status counts.

    Goal:
    Provide one machine-readable artifact per tool run while keeping enough
    metadata to reproduce the command configuration that generated it.
    """

    summary = {
        "ok": sum(1 for item in benchmarks if item.get("status") == "ok"),
        "failed": sum(1 for item in benchmarks if item.get("status") == "failed"),
        "skipped": sum(1 for item in benchmarks if item.get("status") == "skipped"),
        "timeout": sum(1 for item in benchmarks if item.get("status") == "timeout"),
        "dry_run": sum(1 for item in benchmarks if item.get("status") == "dry_run"),
        "total": len(benchmarks),
    }
    return {
        "schema": "hls-benchmark-qor-v1",
        "generated_at": utc_timestamp(),
        "tool": tool,
        "config": config,
        "summary": summary,
        "benchmarks": benchmarks,
    }


def load_openhls_cycle_count(report_path: Path, benchmark_name: str) -> dict[str, Any]:
    """Load the best available OpenHLS cycle count for one benchmark.

    Role:
    Reuse the existing ``benchmarks/report.json`` cycle-count data instead of
    deriving latency from unrelated synthesis or scheduling information.

    Goal:
    Keep the common ``latency.cycles`` field honest: it is populated only when a
    cycle count exists in the OpenHLS report, and its source records the exact
    level selected.
    """

    empty = {"cycles": None, "source": None}
    if not report_path.exists():
        return empty
    try:
        payload = json.loads(report_path.read_text(encoding="utf-8"))
    except Exception:
        return empty
    benchmark = payload.get("benchmarks", {}).get(benchmark_name, {})
    cycles = benchmark.get("qor", {}).get("cycles", {})
    for level in ("rtl", "L4", "L3", "L2"):
        value = _to_int(cycles.get(level))
        if value is not None:
            return {"cycles": value, "source": f"openhls_report:{level}"}
    return empty


def parse_bambu_log(log_path: Path) -> dict[str, Any]:
    """Extract Bambu/PANDA pre-synthesis QoR estimates from one transcript.

    Role:
    Parse the stable summary lines printed by Bambu for estimated area, DSP use,
    BRAM bit size, and estimated maximum frequency.

    Goal:
    Preserve Bambu-native estimate fields in the common schema without treating
    them as post-synthesis Vivado resources.
    """

    text = _read_text(log_path)
    fmax_mhz = _search_float(text, r"Estimated max frequency \(MHz\):\s*([0-9.]+)")
    return _metrics(
        area=_search_int(text, r"Estimated resources area \(no Muxes and address logic\):\s*([0-9,]+)"),
        dsp=_search_int(text, r"Estimated number of DSPs:\s*([0-9,]+)"),
        bram_bits=_search_int(text, r"BRAM bitsize:\s*([0-9,]+)"),
        fmax_mhz=fmax_mhz,
        tclk_ns=_period_from_fmax_mhz(fmax_mhz),
    )


def parse_vivado_compact_log(log_path: Path) -> dict[str, Any]:
    """Extract post-synthesis Vivado metrics from a compact runner log line.

    Role:
    Support existing benchmark logs that summarize Vivado results as one line of
    the form ``vivado synthesis OK: LUT=...``.

    Goal:
    Make Bambu and OpenHLS benchmark reports comparable when the full Vivado
    report directory is unavailable but the compact log summary exists.
    """

    text = _read_text(log_path)
    match = re.search(
        r"vivado synthesis OK: LUT=(\d+) FF=(\d+) BRAM=(\d+) DSP=(\d+) Tclk_est_ps=(\d+)",
        text,
    )
    if not match:
        return _metrics()
    lut, ff, bram, dsp, tclk_ps = match.groups()
    return _metrics(
        lut=_to_int(lut),
        ff=_to_int(ff),
        bram=_to_int(bram),
        dsp=_to_int(dsp),
        tclk_ns=_to_float(tclk_ps) / 1000.0 if _to_float(tclk_ps) is not None else None,
    )


def parse_bambu_vivado_xml(report_path: Path) -> dict[str, Any]:
    """Extract Vivado QoR from Bambu's generated synthesis XML summary.

    Role:
    Read the ``<top>_report.xml`` file written by Bambu's Vivado Tcl flow and map
    Xilinx synthesis summary item identifiers to the normalized resource/timing
    fields.

    Goal:
    Populate Bambu ``post_synth`` metrics from completed Vivado execution instead
    of relying on the older compact OpenHLS log banner.
    """

    if not report_path.is_file():
        return _metrics()
    try:
        root = ET.parse(report_path).getroot()
    except ET.ParseError:
        return _metrics()
    items: dict[str, str] = {}
    for item in root.findall(".//item"):
        key = item.attrib.get("stringID")
        value = item.attrib.get("value")
        if key and value is not None:
            items[key] = value
    return _metrics(
        lut=_to_int(items.get("XILINX_SLICE_LUTS")),
        ff=_to_int(items.get("XILINX_SLICE_REGISTERS")),
        bram=_to_int(items.get("XILINX_BLOCK_RAMFIFO")),
        dsp=_to_int(items.get("XILINX_DSPS")),
        tclk_ns=_to_float(items.get("XILINX_DESIGN_DELAY")),
    )


def parse_vivado_report_dir(vivado_dir: Path) -> dict[str, Any]:
    """Extract post-synthesis Vivado metrics from report files in a directory.

    Role:
    Read the ``util.rpt`` and ``timing.rpt`` files emitted by the LegUp Vivado
    flow and map their data into the common resource/timing fields.

    Goal:
    Share Vivado parsing across runners while keeping missing reports explicit as
    ``None`` values rather than substituting zeros.
    """

    return parse_vivado_report_files(vivado_dir / "util.rpt", vivado_dir / "timing.rpt")


def parse_vivado_report_files(util_path: Path, timing_path: Path) -> dict[str, Any]:
    """Extract post-synthesis Vivado metrics from explicit report paths.

    Role:
    Support flows such as Dynamatic whose Vivado reports are named
    ``utilization_post_syn.rpt`` and ``timing_post_syn.rpt`` rather than the
    compact LegUp ``util.rpt``/``timing.rpt`` pair.

    Goal:
    Reuse one parser for all Vivado report-producing tools while keeping report
    discovery in the tool-specific runner.
    """

    metrics = _metrics()
    util_text = _read_text(util_path)
    timing_text = _read_text(timing_path)

    if util_text:
        metrics["lut"] = _extract_vivado_util(util_text, r"CLB LUTs|Slice LUTs|LUTs")
        metrics["ff"] = _extract_vivado_util(
            util_text,
            r"\|\s*(?:CLB Registers|Slice Registers|Registers)\s*\|",
        )
        metrics["bram"] = _extract_vivado_util(util_text, r"Block RAM Tile|BRAM|RAMB")
        metrics["dsp"] = _extract_vivado_util(util_text, r"DSPs")
    if timing_text:
        metrics["tclk_ns"] = _search_float(timing_text, r"Data Path Delay:\s*([0-9.]+)ns")
        metrics["slack_ns"] = _search_float(timing_text, r"Slack.*?:\s*([-0-9.]+)ns")
    return metrics


def normalize_bambu_result(
    result: Any,
    *,
    cycle_report_path: Path,
    device_name: str,
    period_ns: float | None = None,
) -> dict[str, Any]:
    """Convert one Bambu runner result into the common QoR schema.

    Role:
    Combine the Bambu job result object, its synthesis transcript, optional
    Vivado compact log, and exact Bambu simulation cycle data into one normalized
    benchmark record.

    Goal:
    Preserve Bambu's native estimates separately from implementation metrics and
    label execution-backed cycle counts unambiguously.  The legacy
    ``cycle_report_path`` argument is retained for caller compatibility but is no
    longer used as a latency fallback.
    """

    name = _value(result, "name")
    status = normalize_status(_value(result, "status"))
    log_path = Path(_value(result, "log_path")) if _value(result, "log_path") else None
    benchmark_dir = log_path.parent.parent if log_path is not None else None
    vivado_log = benchmark_dir / ".logs" / "vivado_synth.stdout.log" if benchmark_dir else Path("")
    synthesis_report_path = Path(_value(result, "synthesis_report_path")) if _value(result, "synthesis_report_path") else None
    synthesis_report_dir = Path(_value(result, "synthesis_report_dir")) if _value(result, "synthesis_report_dir") else None
    command = _value(result, "command") or []
    cycles = _to_int(_value(result, "latency_cycles"))
    may_have_current_logs = status not in {"dry_run", "skipped"}
    post_synth = _metrics()
    if may_have_current_logs and synthesis_report_dir:
        post_synth = parse_vivado_report_files(
            synthesis_report_dir / "post_synth_util.rpt",
            synthesis_report_dir / "post_synth_timing_summary.rpt",
        )
    if may_have_current_logs and _is_empty_metrics(post_synth) and synthesis_report_path:
        post_synth = parse_bambu_vivado_xml(synthesis_report_path)
    elif may_have_current_logs and _is_empty_metrics(post_synth):
        post_synth = parse_vivado_compact_log(vivado_log)
    return {
        "benchmark": name,
        "tool": "bambu",
        "status": status,
        "returncode": _value(result, "returncode"),
        "elapsed_seconds": _value(result, "elapsed_seconds"),
        "message": _value(result, "message"),
        "target": {
            "part": device_name,
            "clock_period_ns": period_ns,
        },
        "sources": {
            "source": _value(result, "source"),
            "top_function": _value(result, "top_function"),
        },
        "latency": {
            "cycles": cycles,
            "source": _value(result, "latency_source") if cycles is not None else None,
        },
        "tool_estimate": parse_bambu_log(log_path) if log_path and may_have_current_logs else _metrics(),
        "post_synth": post_synth,
        "artifacts": {
            "command": command,
            "log": str(log_path) if log_path else None,
            "simulation_log": _value(result, "simulation_log_path"),
            "synthesis_log": _value(result, "synthesis_log_path"),
            "synthesis_report": _value(result, "synthesis_report_path"),
            "synthesis_report_dir": _value(result, "synthesis_report_dir"),
            "work_dir": _value(result, "output_dir"),
        },
    }


def normalize_legup_record(
    record: dict[str, Any],
    *,
    part: str,
    period_ns: float,
) -> dict[str, Any]:
    """Convert one LegUp runner record into the common QoR schema.

    Role:
    Map the richer LegUp-native result dictionary to the shared fields while
    keeping LegUp-specific reports in the original JSON emitted by the runner.

    Goal:
    Let comparison scripts consume LegUp and Bambu reports with the same keys
    without losing information from the tool-native artifact.
    """

    name = record.get("benchmark")
    legup = record.get("legup") or {}
    simulation = record.get("simulation") or {}
    vivado = record.get("vivado") or {}
    fmax_mhz = _to_float(legup.get("estimated_fmax_mhz"))
    cycles = _to_int(simulation.get("cycles"))
    return {
        "benchmark": name,
        "tool": "legup",
        "status": normalize_status(record.get("status")),
        "returncode": record.get("returncode"),
        "elapsed_seconds": None,
        "message": record.get("error"),
        "target": {
            "part": part,
            "clock_period_ns": period_ns,
        },
        "sources": {
            "source": (record.get("sources") or {}).get("combined"),
            "top_function": (record.get("sources") or {}).get("top_function"),
        },
        "latency": {
            "cycles": cycles,
            "source": simulation.get("source") if cycles is not None else None,
        },
        "tool_estimate": _metrics(
            area=_to_int(legup.get("logic_elements")),
            lut=_to_int(legup.get("combinational")),
            ff=_to_int(legup.get("registers")),
            dsp=_to_int(legup.get("dsp_elements")),
            fmax_mhz=fmax_mhz,
            tclk_ns=_period_from_fmax_mhz(fmax_mhz),
        ),
        "post_synth": _metrics(
            lut=_to_int(vivado.get("lut")),
            ff=_to_int(vivado.get("ff")),
            bram=_to_int(vivado.get("bram")),
            dsp=_to_int(vivado.get("dsp")),
            tclk_ns=_to_float(vivado.get("tclk_ns")),
            slack_ns=_to_float(vivado.get("slack_ns")),
        ),
        "artifacts": {
            "command": record.get("command") or [],
            "log": (record.get("paths") or {}).get("log"),
            "work_dir": (record.get("paths") or {}).get("work_build"),
        },
    }


def normalize_status(status: Any) -> str:
    """Map runner-specific status strings to the common status vocabulary.

    Role:
    Accept status labels from existing scripts without forcing those scripts to
    rename their user-facing PASS/FAIL/ok/skipped values.

    Goal:
    Provide predictable normalized values for dashboards and regressions:
    ``ok``, ``failed``, ``skipped``, ``timeout``, or ``dry_run``.
    """

    lowered = str(status or "").strip().lower()
    if lowered in {"pass", "ok", "success"}:
        return "ok"
    if lowered in {"skip", "skipped"}:
        return "skipped"
    if lowered in {"timeout", "timed out"}:
        return "timeout"
    if lowered in {"dry-run", "dry_run", "dry run"}:
        return "dry_run"
    return "failed" if lowered else "skipped"


def _metrics(**overrides: Any) -> dict[str, Any]:
    """Create a complete metric dictionary with explicit missing values.

    Role:
    Ensure every estimate or post-synthesis block exposes the same keys even
    when a particular tool does not report all metrics.

    Goal:
    Avoid downstream key checks and make absent data visibly different from zero.
    """

    metrics = {
        "area": None,
        "lut": None,
        "ff": None,
        "dsp": None,
        "bram": None,
        "bram_bits": None,
        "fmax_mhz": None,
        "tclk_ns": None,
        "slack_ns": None,
    }
    metrics.update(overrides)
    return metrics


def _is_empty_metrics(metrics: dict[str, Any]) -> bool:
    """Return true when a normalized metric block has no reported values.

    Role:
    Distinguish genuinely missing QoR data from valid zero-valued resources such
    as BRAM=0 or DSP=0.

    Goal:
    Let parsers fall back to alternate report formats only when the preferred
    report source contained no usable information.
    """

    return all(value is None for value in metrics.values())


def _read_text(path: Path) -> str:
    """Read a text file defensively for report parsing.

    Role:
    Hide file-existence and encoding details from the individual parsers.

    Goal:
    Make missing logs produce missing metrics rather than exceptions in summary
    generation after partial or failed batch runs.
    """

    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def _search_int(text: str, pattern: str) -> int | None:
    """Return the first integer captured by a regular expression pattern.

    Role:
    Provide a small helper for conventional synthesis-report text parsing.

    Goal:
    Keep each metric parser concise while preserving ``None`` for absent fields.
    """

    match = re.search(pattern, text)
    return _to_int(match.group(1)) if match else None


def _search_float(text: str, pattern: str) -> float | None:
    """Return the first floating-point value captured by a pattern.

    Role:
    Centralize numeric conversion for timing and frequency report fields.

    Goal:
    Treat malformed or absent values as missing data rather than crashing common
    report generation.
    """

    match = re.search(pattern, text)
    return _to_float(match.group(1)) if match else None


def _extract_vivado_util(text: str, pattern: str) -> int | None:
    """Extract the first numeric utilization cell from a Vivado table row.

    Role:
    Interpret Vivado's pipe-delimited utilization reports for LUT, FF, BRAM, and
    DSP rows selected by the provided row-name pattern.

    Goal:
    Share the same post-synthesis resource parser between LegUp and future flows
    that generate standard Vivado report files.
    """

    for line in text.splitlines():
        if not re.search(pattern, line):
            continue
        cells = [cell.strip().replace(",", "") for cell in line.split("|")]
        nums = [cell for cell in cells if re.fullmatch(r"-?\d+(?:\.\d+)?", cell)]
        if nums:
            return _to_int(nums[0])
    return None


def _period_from_fmax_mhz(fmax_mhz: float | None) -> float | None:
    """Convert a maximum frequency in MHz to a clock period in ns.

    Role:
    Normalize tools that report estimated frequency instead of period.

    Goal:
    Provide a comparable timing field while leaving the original frequency value
    present in the same metric block.
    """

    if fmax_mhz is None or fmax_mhz == 0:
        return None
    return 1000.0 / fmax_mhz


def _to_int(value: Any) -> int | None:
    """Convert a report value to an integer when possible.

    Role:
    Accept integer strings with commas and numeric Python values from different
    parsers.

    Goal:
    Keep missing or malformed values as ``None`` instead of inventing defaults.
    """

    if value is None:
        return None
    try:
        return int(float(str(value).replace(",", "").strip()))
    except ValueError:
        return None


def _to_float(value: Any) -> float | None:
    """Convert a report value to a float when possible.

    Role:
    Normalize timing and frequency fields that may arrive as strings or numbers.

    Goal:
    Preserve absent values as ``None`` for honest downstream comparisons.
    """

    if value is None:
        return None
    try:
        return float(str(value).replace(",", "").strip())
    except ValueError:
        return None


def _value(obj: Any, name: str) -> Any:
    """Read a named field from either an object or a dictionary.

    Role:
    Let the normalization code accept dataclass instances from Bambu and plain
    dictionaries from LegUp without duplicating access logic.

    Goal:
    Keep runner integration minimal and additive.
    """

    if isinstance(obj, dict):
        return obj.get(name)
    return getattr(obj, name, None)
