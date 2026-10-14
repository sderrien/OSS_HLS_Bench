#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, asdict

from hls_bench import qor as common_qor
from hls_bench import inputs as common_inputs
from hls_bench import loop_labels
from hls_bench import paths


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
RTL_SIMULATION_TIMEOUT_SECONDS = 120
DEFAULT_OPENHLS_ROOT = paths.BENCHMARKS_ROOT
DEFAULT_WORK_ROOT = paths.LEGUP_WORK_ROOT
DEFAULT_NORMALIZED_OUTPUT = DEFAULT_WORK_ROOT / "legup_qor.json"


@dataclass
class SourceSet:
    wrapper: str | None
    kernel: str | None
    combined: str | None = None
    cxx_mode: bool = False
    top_function: str | None = None


def read_json(path: pathlib.Path) -> dict:
    return json.loads(path.read_text())


def write_json(path: pathlib.Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def first_existing(paths: list[pathlib.Path]) -> pathlib.Path | None:
    for path in paths:
        if path.is_file():
            return path
    return None


def list_benchmarks(root: pathlib.Path) -> list[str]:
    manifest = root / "manifest.json"
    if manifest.exists():
        data = read_json(manifest)
        benches = list((data.get("kernels") or {}).keys())
        if benches:
            return benches
    return sorted(p.name for p in root.iterdir() if p.is_dir() and (p / "src").is_dir())


def detect_sources(benchdir: pathlib.Path) -> SourceSet:
    srcdir = benchdir / "src" if (benchdir / "src").is_dir() else benchdir
    wrapper = first_existing([
        *sorted(srcdir.glob("main_*.c")),
        *sorted(srcdir.glob("main_*.cpp")),
        srcdir / "main.c",
        srcdir / "main.cpp",
    ])

    kernel_candidates = [
        *sorted(srcdir.glob("*_kernel.c")),
        *sorted(srcdir.glob("*_kernel.cpp")),
        *sorted(srcdir.glob("*.c")),
        *sorted(srcdir.glob("*.cpp")),
    ]
    if wrapper is not None:
        kernel_candidates = [p for p in kernel_candidates if p.resolve() != wrapper.resolve()]

    kernel = first_existing(kernel_candidates)

    if wrapper is None and kernel is not None:
        wrapper = kernel

    cxx_mode = any(p.suffix == ".cpp" for p in (wrapper, kernel) if p is not None)
    return SourceSet(wrapper=str(wrapper) if wrapper else None, kernel=str(kernel) if kernel else None, cxx_mode=cxx_mode)


def split_main(text: str) -> tuple[str, str]:
    pre: list[str] = []
    main: list[str] = []
    in_main = False
    main_re = re.compile(r"^int[\t ]+main[\t ]*\(")
    for line in text.splitlines(keepends=True):
        if not in_main and main_re.search(line):
            in_main = True
        if in_main:
            main.append(line)
        else:
            pre.append(line)
    return "".join(pre), "".join(main)


def find_hls_top_function(text: str) -> str | None:
    lines = text.splitlines()
    func_re = re.compile(r"\b(?:void|int|float|double|char|short|long|unsigned|signed|uint\d+_t|int\d+_t)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    for idx, line in enumerate(lines):
        lowered = line.lower()
        has_top_pragma = (
            "#pragma hls top" in lowered
            or "#pragma hls toplevel" in lowered
            or "#pragma shls top" in lowered
            or "#pragma shls toplevel" in lowered
        )
        if not has_top_pragma and 'annotate("hls.toplevel")' not in line:
            continue
        for next_line in lines[idx : min(idx + 80, len(lines))]:
            m = func_re.search(next_line)
            if m:
                return m.group(1)
    for line in lines:
        m = re.search(r'__attribute__\s*\(\(annotate\("hls\.toplevel"\)\)\)\s*(?:void|int|float|double|char|short|long|unsigned|signed|uint\d+_t|int\d+_t)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(', line)
        if m:
            return m.group(1)
    return None


def make_kernel_source(prepdir: pathlib.Path, kernel: pathlib.Path, benchmark: str) -> pathlib.Path:
    """Stage one label-annotated kernel source for LegUp synthesis.

    Role:
    Remove host-only main code, preserve source loop labels, and write the exact
    translation unit passed to the LegUp flow.

    Goal:
    Keep RV32I functional RTL simulation practical by removing its diagnostic
    instruction trace from synthesized hardware without changing instruction
    decoding, memory contents, or control behavior.
    """

    kernel_text, _ = split_main(kernel.read_text())
    if benchmark == "rv32i":
        kernel_text = "#define NOTRACE 1\n" + kernel_text
    synth_source = prepdir / "kernel_only.c"
    synth_source.write_text(loop_labels.label_c_source_text(kernel_text)[0])
    return synth_source


def stage_benchmark(repo_root: pathlib.Path, openhls_root: pathlib.Path, work_root: pathlib.Path, bench: str) -> tuple[SourceSet, pathlib.Path, pathlib.Path, pathlib.Path | None]:
    benchdir = openhls_root / bench
    bench_root = work_root / bench
    prepdir = bench_root / "src"
    builddir = bench_root / "build"

    if bench_root.exists():
        shutil.rmtree(bench_root)
    prepdir.mkdir(parents=True, exist_ok=True)
    builddir.mkdir(parents=True, exist_ok=True)

    sources = detect_sources(benchdir)
    if sources.wrapper is None:
        raise FileNotFoundError(f"no wrapper source file found for {bench}")
    if sources.kernel is None:
        sources.kernel = sources.wrapper

    wrapper = pathlib.Path(sources.wrapper)
    kernel = pathlib.Path(sources.kernel)

    shutil.copy2(wrapper, prepdir / "wrapper.c")
    shutil.copy2(kernel, prepdir / "kernel.c")
    shutil.copy2(wrapper, prepdir / "main_legup.c")
    shutil.copy2(kernel, prepdir / "kernel_legup.c")

    src_dir = wrapper.parent
    for hdr in sorted(src_dir.glob("*.h")):
        shutil.copy2(hdr, prepdir / hdr.name)
    for hdr in sorted(src_dir.glob("*.hpp")):
        shutil.copy2(hdr, prepdir / hdr.name)

    include_dir = src_dir / "include"
    if include_dir.is_dir():
        shutil.copytree(include_dir, prepdir / "include", dirs_exist_ok=True)

    shared_include = openhls_root.parent.parent / "library" / "include"
    if shared_include.is_dir():
        shutil.copytree(shared_include, prepdir / "include", dirs_exist_ok=True)

    synth_source = make_kernel_source(prepdir, prepdir / "kernel.c", bench)
    sources.combined = str(synth_source)
    shared_inputs = common_inputs.get_benchmark_inputs(bench)
    sources.top_function = find_hls_top_function(synth_source.read_text())
    if sources.top_function is None and shared_inputs is not None:
        sources.top_function = shared_inputs.top_function
    return sources, prepdir, builddir, synth_source


def run_legup_flow(
    repo_root: pathlib.Path,
    synth_source: pathlib.Path,
    builddir: pathlib.Path,
    part: str,
    period: float,
    image: str,
    legup_script: pathlib.Path,
    run_vivado: bool,
    cxx_mode: bool,
    vivado_cmd: str,
    preserve_function: str,
    compile_defines: list[str],
) -> tuple[int, list[str], pathlib.Path]:
    cmd = [
        str(legup_script),
        "--image",
        image,
        "--part",
        part,
        "--period",
        str(period),
        "--workdir",
        str(builddir),
        "--preserve-function",
        preserve_function,
    ]
    if not run_vivado:
        cmd.append("--skip-vivado")
    if cxx_mode:
        cmd.append("--cxx")
    if run_vivado:
        cmd.extend(["--vivado", vivado_cmd])
    for compile_define in compile_defines:
        cmd.extend(["--compile-define", compile_define])
    cmd.append(str(synth_source))

    log_path = builddir.parent / "run.log"
    builddir.mkdir(parents=True, exist_ok=True)
    with log_path.open("w") as log:
        proc = subprocess.run(
            cmd,
            cwd=repo_root,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
    return proc.returncode, cmd, log_path


def parse_pipeline_report(path: pathlib.Path) -> list[dict]:
    if not path.exists():
        return []
    loops: list[dict] = []
    current: dict | None = None
    label_re = re.compile(r'^Generating Loop Pipeline for label:\s*"(?P<label>.*)"\s*$')
    kv_re = re.compile(r"^(?P<key>[A-Za-z ]+):\s*(?P<val>.*)$")
    for raw in path.read_text(errors="ignore").splitlines():
        line = raw.strip()
        if not line:
            continue
        m = label_re.match(line)
        if m:
            if current:
                loops.append(current)
            current = {
                "label": m.group("label"),
            }
            continue
        if current is None:
            continue
        m = kv_re.match(line)
        if not m:
            continue
        key = m.group("key").strip()
        val = m.group("val").strip()
        current[key] = val
    if current:
        loops.append(current)

    parsed: list[dict] = []
    for loop in loops:
        ii = _to_int(loop.get("II"))
        time = _to_int(loop.get("Time"))
        max_stage = _to_int(loop.get("maxStage"))
        parsed.append(
            {
                "label": loop.get("label"),
                "bb": loop.get("BB"),
                "ii": ii,
                "depth_cycles": time,
                "max_stage": max_stage,
                "stages": None if max_stage is None else max_stage + 1,
                "induction_var": loop.get("Induction var"),
            }
        )
    return parsed


def parse_schedule_info(builddir: pathlib.Path, verilog_path: pathlib.Path | None, loops: list[dict]) -> dict:
    """Collect LegUp schedule information into a compact machine-readable block.

    LegUp 4.0 does not consistently emit a single Vitis-style latency table for
    every design.  This parser therefore combines the reliable loop pipeline
    report with best-effort static data from scheduling reports and generated
    Verilog.  The resulting fields make it clear when an exact total cycle count
    is unavailable while still exposing useful loop II/depth and FSM size data.
    """
    ii_values = [loop.get("ii") for loop in loops if loop.get("ii") is not None]
    depth_values = [loop.get("depth_cycles") for loop in loops if loop.get("depth_cycles") is not None]
    explicit_cycles = None
    explicit_source = None
    for rpt in sorted(builddir.glob("*.rpt")):
        parsed = parse_explicit_cycle_count(rpt)
        if parsed is not None:
            explicit_cycles = parsed
            explicit_source = str(rpt)
            break

    return {
        "cycle_count_static": explicit_cycles,
        "cycle_count_source": explicit_source,
        "fsm_states": parse_fsm_state_count(verilog_path),
        "pipelined_loop_count": len(loops),
        "min_ii": min(ii_values) if ii_values else None,
        "max_ii": max(ii_values) if ii_values else None,
        "min_depth_cycles": min(depth_values) if depth_values else None,
        "max_depth_cycles": max(depth_values) if depth_values else None,
        "loops": loops,
    }


def run_legup_cycle_simulation(builddir: pathlib.Path, synth_source: pathlib.Path, verilator_cmd: str, benchmark: str) -> dict:
    """Run the generated LegUp RTL testbench and extract an exact cycle count.

    Role:
    Prepare a simulation-only Verilog copy from LegUp's generated RTL, build it
    with Verilator, execute the generated ``main_tb`` testbench, and capture the
    ``Cycles:`` line printed when the hardware ``finish`` signal is asserted.

    Goal:
    Populate normalized latency only from RTL execution rather than from LegUp
    schedule estimates, loop depths, or post-synthesis timing reports.
    """

    # Verilator executes with ``builddir`` as its working directory.  Resolve
    # every staged path first so callers may use repository-relative work roots
    # without accidentally prefixing those paths a second time during RTL build.
    # This preserves the generated RTL and testbench rather than skipping cycle
    # simulation when the batch runner is invoked from the repository root.
    builddir = builddir.resolve()
    synth_source = synth_source.resolve()
    verilog_path = builddir / f"{synth_source.stem}.v"
    sim_verilog_path = builddir / f"{synth_source.stem}_cycle_sim.v"
    log_path = builddir / "cycle_sim.log"
    obj_dir = builddir / "obj_cycle_sim"
    result = {
        "status": "skipped",
        "cycles": None,
        "source": None,
        "verilog": str(sim_verilog_path),
        "log": str(log_path),
        "command": [],
        "message": None,
    }
    if not verilog_path.is_file():
        result["message"] = f"generated Verilog not found: {verilog_path}"
        return result

    sim_text = prepare_legup_cycle_sim_verilog(verilog_path.read_text(errors="ignore"), benchmark, builddir)
    sim_verilog_path.write_text(sim_text)
    command = [
        verilator_cmd,
        "-Wno-fatal",
        "--binary",
        "--top-module",
        "main_tb",
        "--Mdir",
        str(obj_dir),
        str(sim_verilog_path),
    ]
    command.extend(str(path) for path in _legup_cycle_sim_extra_verilog(builddir))
    result["command"] = command
    env = _verilator_env(verilator_cmd)
    with log_path.open("w", encoding="utf-8") as log:
        build = subprocess.run(command, cwd=builddir, stdout=log, stderr=subprocess.STDOUT, text=True, env=env)
        if build.returncode != 0:
            result["status"] = "failed"
            result["message"] = f"Verilator build failed with exit code {build.returncode}"
            return result
        executable = obj_dir / "Vmain_tb"
        try:
            run = subprocess.run(
                [str(executable)],
                cwd=builddir,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                env=env,
                timeout=RTL_SIMULATION_TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired:
            result["status"] = "failed"
            result["message"] = f"Verilator simulation timed out after {RTL_SIMULATION_TIMEOUT_SECONDS}s"
            return result
        if run.returncode != 0:
            result["status"] = "failed"
            result["message"] = f"Verilator simulation failed with exit code {run.returncode}"
            return result

    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    if "%Fatal:" in log_text or "Assertion failed" in log_text:
        result["status"] = "failed"
        result["message"] = "Verilator simulation reported a fatal assertion"
        return result

    cycles = parse_legup_cycle_sim_log(log_path)
    if cycles is None:
        result["status"] = "failed"
        result["message"] = "Verilator simulation completed without a Cycles line"
        return result
    result["status"] = "ok"
    result["cycles"] = cycles
    result["source"] = f"{log_path}:Cycles"
    return result


def _legup_cycle_sim_extra_verilog(builddir: pathlib.Path) -> list[pathlib.Path]:
    """Return extra Verilog files needed by LegUp RTL cycle simulation.

    Role:
    Collect support modules that LegUp references from generated kernels but does
    not inline into ``kernel_only.v``, especially floating-point operator aliases
    and Altera library implementations.

    Goal:
    Let Verilator elaborate floating-point benchmarks such as ``spmv`` without
    changing the kernel or replacing FP operators with behavioral fallbacks that
    would hide missing RTL dependencies.
    """

    files: list[pathlib.Path] = []
    aliases = builddir / "legup_fp_aliases.v"
    if aliases.is_file():
        files.append(aliases)
    if (builddir / "legup_xilinx_fp.v").is_file():
        files.append(REPO_ROOT / "hls_bench" / "legup_xilinx_fp_model.sv")
        files.append(builddir / "legup_xilinx_fp.v")

    generated_rtl = builddir / "kernel_only.v"
    files.extend(_legup_fp_implementation_files(aliases, generated_rtl))
    return files


def _legup_fp_implementation_files(*verilog_files: pathlib.Path) -> list[pathlib.Path]:
    """Resolve LegUp FP alias implementations for Verilator.

    Role:
    Read generated Verilog support files and find concrete ``altfp_*`` modules
    that are instantiated either through local aliases or directly by LegUp's
    generated RTL, then map those module names to LegUp's local Altera FP
    library files.

    Goal:
    Include only the floating-point implementation files required by the current
    kernel.  Pulling in the whole Altera simulation library also imports device
    atom models that Verilator rejects, so this keeps exact RTL simulation on the
    actual referenced FP RTL without unrelated unsupported modules.  Some kernels
    such as ``lbm`` instantiate ``altfp_divider_33`` directly in ``kernel_only.v``
    instead of through ``legup_fp_aliases.v``, so the generated RTL must be scanned
    as well as the alias file.
    """

    module_names: set[str] = set()
    for verilog_file in verilog_files:
        if not verilog_file.is_file():
            continue
        text = verilog_file.read_text(encoding="utf-8", errors="replace")
        module_names.update(re.findall(r"\b(altfp_[A-Za-z0-9_]+)\s+impl\b", text))
        module_names.update(re.findall(r"^\s*(altfp_[A-Za-z0-9_]+)\s+[A-Za-z_][A-Za-z0-9_$]*\s*\(", text, re.MULTILINE))
    altera_lib = REPO_ROOT / "legup-4.0" / "ip" / "libs" / "altera"
    files: list[pathlib.Path] = []
    for module_name in sorted(module_names):
        path = altera_lib / f"{module_name}.v"
        if path.is_file():
            files.append(path)
    return files


def prepare_legup_cycle_sim_verilog(text: str, benchmark: str | None = None, builddir: pathlib.Path | None = None) -> str:
    """Return a Verilator-ready simulation copy of LegUp-generated Verilog.

    Role:
    Repair only the generated wrapper/testbench issues that prevent executing the
    RTL testbench outside LegUp's original simulator setup: missing top-level
    pointer argument ties, stale void-return connections, and a non-portable clock
    generator style.

    Goal:
    Keep the synthesized RTL logic intact while making the existing ``main_tb``
    runnable so the cycle count comes from actual ``finish`` assertion timing.
    """

    shared_inputs = common_inputs.get_benchmark_inputs(benchmark or "")
    main_ports = _module_ports(text, "main")
    connected_ports = _instance_ports(text, "main", "main_inst")
    missing_arg_ports = [port for port in main_ports if port.startswith("arg_") and port not in connected_ports]
    text = _rewrite_main_instance(text, main_ports, missing_arg_ports)
    text = _insert_top_simulation_ties(text, main_ports, missing_arg_ports, shared_inputs)
    text = _replace_memory_controller_for_simulation(text, shared_inputs)
    if benchmark == "rv32i":
        text = _insert_rv32i_result_assertion(text)
    text = _convert_mif_initializers_for_verilator(text, builddir)
    text = _append_altera_memory_initialization_stub(text)
    text = re.sub(r"always\s*@\s*\(\s*clk\s*\)\s*\n\s*clk\s*<=\s*#10\s*~clk\s*;", "always #10 clk = ~clk;", text)
    return text


def _insert_rv32i_result_assertion(text: str) -> str:
    """Assert the RV32I GCD program's memory-mapped result in the RTL testbench.

    Role:
    Add a benchmark-specific check to LegUp's generated ``main_tb`` after the
    processor asserts ``finish``.

    Goal:
    Reject a simulation that merely terminates, such as the former zero-filled
    instruction-memory run.  The five argument regions occupy 4 KiB slots in the
    replacement memory controller; GCD writes its result and pass marker to byte
    addresses 520 and 524 of four little-endian data byte lanes.
    """

    cycle_marker = '        $display("Cycles: %d", ($time-50)/20);'
    if cycle_marker not in text:
        return text
    result_check = """        // mem0 is the least-significant byte lane, matching cpu_run's load/store logic.
        if ({top_inst.memory_controller_inst.sim_mem[16514], top_inst.memory_controller_inst.sim_mem[12418], top_inst.memory_controller_inst.sim_mem[8322], top_inst.memory_controller_inst.sim_mem[4226]} !== 32'd3 ||
             {top_inst.memory_controller_inst.sim_mem[16515], top_inst.memory_controller_inst.sim_mem[12419], top_inst.memory_controller_inst.sim_mem[8323], top_inst.memory_controller_inst.sim_mem[4227]} !== 32'd1) begin
            $fatal(1, "RV32I GCD check failed");
        end
"""
    return text.replace(cycle_marker, result_check + cycle_marker, 1)


def _convert_mif_initializers_for_verilator(text: str, builddir: pathlib.Path | None) -> str:
    """Convert LegUp MIF memory initializers into Verilator-readable hex files.

    Role:
    LegUp RAM/ROM modules reference Altera ``.mif`` files through ``init_file``
    parameters, and their simulation code expects the Altera helper to convert
    those files before ``$readmemh``.  Verilator does not understand the MIF
    syntax directly.

    Goal:
    Preserve the generated memory contents for exact RTL cycle simulation by
    translating each referenced MIF file into a plain one-word-per-line hex file
    and retargeting only the simulation copy of the Verilog to that file.
    """

    if builddir is None:
        return text
    replacements: dict[str, str] = {}
    for name in sorted(set(re.findall(r'"([^"/]+\.mif)"', text))):
        if name == "UNUSED.mif":
            continue
        mif_path = builddir / name
        if not mif_path.is_file():
            continue
        hex_name = f"{mif_path.stem}.verhex"
        _write_verilator_hex_from_mif(mif_path, builddir / hex_name)
        replacements[name] = hex_name
    for mif_name, hex_name in replacements.items():
        text = text.replace(f'"{mif_name}"', f'"{hex_name}"')
    return text


def _write_verilator_hex_from_mif(mif_path: pathlib.Path, hex_path: pathlib.Path) -> None:
    """Write a ``$readmemh`` file equivalent to a LegUp-generated MIF file.

    Role:
    Parse the small address/data subset emitted by LegUp for initialized RAMs,
    including optional ``Depth =`` metadata and ``address: data;`` content lines.

    Goal:
    Avoid relying on Altera simulator libraries while keeping initialized string
    constants and local memories identical to the LegUp-generated artifacts.
    """

    depth: int | None = None
    values: dict[int, str] = {}
    for raw in mif_path.read_text(encoding="utf-8", errors="replace").splitlines():
        depth_match = re.match(r"\s*Depth\s*=\s*(\d+)\s*;", raw, re.IGNORECASE)
        if depth_match:
            depth = int(depth_match.group(1))
            continue
        data_match = re.match(r"\s*(\d+)\s*:\s*([0-9A-Fa-f]+)\s*;", raw)
        if data_match:
            values[int(data_match.group(1))] = data_match.group(2).lower()
    if depth is None:
        depth = max(values.keys(), default=-1) + 1
    lines = [values.get(index, "0") for index in range(depth)]
    hex_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _append_altera_memory_initialization_stub(text: str) -> str:
    """Provide the Altera RAM initialization helper expected by LegUp RAMs.

    Role:
    LegUp emits simulation-only RAM blocks that instantiate
    ``ALTERA_MF_MEMORY_INITIALIZATION`` and call ``convert_to_ver_file`` before
    ``$readmemh``.  That helper exists in Altera/ModelSim simulation libraries,
    but is not available when we run the generated design through Verilator.

    Goal:
    Let Verilator elaborate the generated RAM modules without hiding memory
    behavior.  For non-``UNUSED`` init files, the helper passes the file name
    through unchanged so the existing ``$readmemh`` remains responsible for
    loading data.
    """

    if "ALTERA_MF_MEMORY_INITIALIZATION" not in text:
        return text
    if re.search(r"\bmodule\s+ALTERA_MF_MEMORY_INITIALIZATION\b", text):
        return text
    stub = r'''

module ALTERA_MF_MEMORY_INITIALIZATION;
  task convert_to_ver_file;
    input [8*256:1] init_file;
    input integer width;
    output [8*256:1] ram_ver_file;
    begin
      ram_ver_file = init_file;
    end
  endtask
endmodule
'''
    return text + stub


def _module_ports(text: str, module_name: str) -> list[str]:
    """List declared ports for a generated Verilog module header.

    Role:
    Read the simple comma-separated ANSI-style module headers emitted by LegUp.

    Goal:
    Let the simulation adapter detect whether ``main`` actually has a return port
    and which pointer arguments must be tied at the generated top level.
    """

    match = re.search(rf"\bmodule\s+{re.escape(module_name)}\s*\((?P<ports>.*?)\)\s*;", text, re.DOTALL)
    if not match:
        return []
    ports: list[str] = []
    for raw in match.group("ports").split(","):
        name = raw.strip()
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            ports.append(name)
    return ports


def _instance_ports(text: str, module_name: str, instance_name: str) -> set[str]:
    """Return named ports connected on one generated Verilog instance.

    Role:
    Inspect the ``main main_inst`` connection list already emitted inside LegUp's
    ``top`` module.

    Goal:
    Add only genuinely missing pointer-argument ties and leave existing memory,
    clock, reset, and control connections untouched.
    """

    match = re.search(rf"\b{re.escape(module_name)}\s+{re.escape(instance_name)}\s*\((?P<body>.*?)\)\s*;", text, re.DOTALL)
    if not match:
        return set()
    return set(re.findall(r"\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(", match.group("body")))


def _rewrite_main_instance(text: str, main_ports: list[str], missing_arg_ports: list[str]) -> str:
    """Rewrite the generated ``main_inst`` connection list for simulation.

    Role:
    Remove connections to ports that the generated ``main`` module does not
    declare, then append deterministic top-level wires for any missing pointer
    arguments.

    Goal:
    Fix LegUp wrapper mismatches without editing the kernel FSM or memory logic
    whose execution time is being measured.
    """

    match = re.search(r"\bmain\s+main_inst\s*\((?P<body>.*?)\)\s*;", text, re.DOTALL)
    if not match:
        return text
    valid_ports = set(main_ports)
    pairs = re.findall(r"\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*([^()]+?)\s*\)", match.group("body"))
    kept = [(port, expr) for port, expr in pairs if port in valid_ports]
    kept.extend((port, port) for port in missing_arg_ports)
    rendered = "main main_inst(\n" + ",\n".join(f"\t.{port}( {expr.strip()} )" for port, expr in kept) + "\n);"
    return text[: match.start()] + rendered + text[match.end() :]


def _insert_top_simulation_ties(
    text: str,
    main_ports: list[str],
    missing_arg_ports: list[str],
    shared_inputs: common_inputs.BenchmarkInputs | None,
) -> str:
    """Declare simulation-only top-level ties required by ``main_inst``.

    Role:
    Insert constant base addresses for pointer arguments and a zero return value
    for void kernels whose generated top wrapper still exposes ``return_val``.

    Goal:
    Allow the generated memory controller and testbench to run to completion so
    latency is measured from ``start`` to ``finish`` in RTL simulation.
    """

    insertion: list[str] = []
    if "return_val" not in set(main_ports):
        insertion.append("assign return_val = 32'd0;")
    scalar_ties = common_inputs.legup_scalar_ties(shared_inputs) if shared_inputs else {}
    memory_regions = common_inputs.legup_memory_regions(shared_inputs) if shared_inputs else {}
    memory_index = 0
    for index, port in enumerate(missing_arg_ports):
        if port in scalar_ties:
            value = scalar_ties[port] & 0xFFFFFFFF
            insertion.append(f"wire [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] {port} = 32'd{value};")
            continue
        if port in memory_regions:
            insertion.append(f"wire [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] {port} = 32'd{memory_index * 4096};")
            memory_index += 1
            continue
        insertion.append(f"wire [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] {port} = 32'd{index + 1};")
    if not insertion:
        return text
    match = re.search(r"output\s+wire\s+\[31:0\]\s+return_val\s*;", text)
    if not match:
        return text
    return text[: match.end()] + "\n" + "\n".join(insertion) + text[match.end() :]


def _replace_memory_controller_for_simulation(
    text: str,
    shared_inputs: common_inputs.BenchmarkInputs | None,
) -> str:
    """Install a byte-addressed simulation memory for shared LegUp inputs.

    Role:
    Replace LegUp's empty external-memory shim with a deterministic Verilator-only
    model initialized from ``hls_bench.inputs``.

    Goal:
    Make LegUp pointer/array arguments observe the same bytes that Bambu and
    Dynamatic pass through their generated C testbenches, while still measuring
    cycles from the RTL ``finish`` signal.
    """

    if shared_inputs is None:
        return text
    regions = common_inputs.legup_memory_regions(shared_inputs)
    if not regions:
        return text

    module_match = re.search(r"\bmodule\s+memory_controller\s*\((?P<body>.*?)\nendmodule", text, re.DOTALL)
    if not module_match:
        return text

    init_lines = _legup_memory_initializers(regions)
    replacement = f"""module memory_controller
(
	clk,
	memory_controller_address_a,
	memory_controller_address_b,
	memory_controller_enable_a,
	memory_controller_enable_b,
	memory_controller_write_enable_a,
	memory_controller_write_enable_b,
	memory_controller_in_a,
	memory_controller_in_b,
	memory_controller_size_a,
	memory_controller_size_b,
	memory_controller_waitrequest,
	memory_controller_out_reg_a,
	memory_controller_out_reg_b
);

parameter latency = 1;
parameter ram_latency = latency;

integer j;
input clk;
input memory_controller_waitrequest;
input [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] memory_controller_address_a;
input memory_controller_enable_a;
input memory_controller_write_enable_a;
input [64-1:0] memory_controller_in_a;
input [1:0] memory_controller_size_a;
output reg [64-1:0] memory_controller_out_reg_a;
input [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] memory_controller_address_b;
input memory_controller_enable_b;
input memory_controller_write_enable_b;
input [64-1:0] memory_controller_in_b;
input [1:0] memory_controller_size_b;
output reg [64-1:0] memory_controller_out_reg_b;

reg [7:0] sim_mem [0:32767];
reg [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] read_address_a;
reg [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] read_address_b;

function [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] sim_addr;
  input [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] addr;
  begin
    // LegUp encodes local RAM objects with a nonzero high address tag.  Keep
    // their low offset separate from the external argument RAMs in sim_mem.
    if (addr[31:23] != 9'd0)
      sim_addr = 32'd24576 + addr[22:0];
    else
      sim_addr = addr;
  end
endfunction

function [63:0] load64;
  input [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] addr;
  reg [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] base;
  begin
    base = sim_addr(addr);
    load64 = {{sim_mem[base + 7], sim_mem[base + 6], sim_mem[base + 5], sim_mem[base + 4],
               sim_mem[base + 3], sim_mem[base + 2], sim_mem[base + 1], sim_mem[base]}};
  end
endfunction

task store_bytes;
  input [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] addr;
  input [63:0] data;
  input [1:0] size;
  reg [`MEMORY_CONTROLLER_ADDR_SIZE-1:0] base;
  integer width;
  begin
    base = sim_addr(addr);
    case (size)
      2'd0: width = 1;
      2'd1: width = 2;
      2'd2: width = 4;
      default: width = 8;
    endcase
    for (j = 0; j < width; j = j + 1)
      sim_mem[base + j] = data[j * 8 +: 8];
  end
endtask

initial begin
  read_address_a = 0;
  read_address_b = 0;
  for (j = 0; j < 32768; j = j + 1)
    sim_mem[j] = 8'd0;
{init_lines}
end

always @(*) begin
  // The RTL consumes a request's result in the next FSM state.  Registering
  // request addresses preserves that one-cycle memory-response contract.
  memory_controller_out_reg_a = load64(read_address_a);
  memory_controller_out_reg_b = load64(read_address_b);
end

always @(posedge clk) begin
  if (!memory_controller_waitrequest) begin
    if (memory_controller_enable_a)
      read_address_a <= memory_controller_address_a;
    if (memory_controller_enable_b)
      read_address_b <= memory_controller_address_b;
    if (memory_controller_enable_a && memory_controller_write_enable_a)
      store_bytes(memory_controller_address_a, memory_controller_in_a, memory_controller_size_a);
    if (memory_controller_enable_b && memory_controller_write_enable_b)
      store_bytes(memory_controller_address_b, memory_controller_in_b, memory_controller_size_b);
  end
end

endmodule"""
    return text[: module_match.start()] + replacement + text[module_match.end() :]


def _legup_memory_initializers(regions: dict[str, bytes]) -> str:
    """Render Verilog byte assignments for shared LegUp memory regions."""

    lines: list[str] = []
    for region_index, (_port, data) in enumerate(regions.items()):
        base = region_index * 4096
        for offset, byte in enumerate(data):
            lines.append(f"  sim_mem[{base + offset}] = 8'h{byte:02x};")
    return "\n".join(lines)


def parse_legup_cycle_sim_log(log_path: pathlib.Path) -> int | None:
    """Parse the exact cycle count printed by the LegUp RTL testbench.

    Role:
    Read the Verilator execution transcript produced by
    ``run_legup_cycle_simulation`` and extract the integer following ``Cycles:``.

    Goal:
    Keep normalized latency tied to an execution-backed simulator artifact.
    """

    if not log_path.is_file():
        return None
    match = re.search(r"\bCycles:\s*(\d+)", log_path.read_text(encoding="utf-8", errors="replace"))
    return _to_int(match.group(1)) if match else None


def _verilator_env(verilator_cmd: str) -> dict[str, str] | None:
    """Prefer a coherent local Verilator installation for RTL simulation.

    Role:
    Add the selected Verilator's directory to ``PATH`` when an absolute command is
    provided, matching the repository's Dynamatic simulation setup.

    Goal:
    Avoid mixing Verilator binaries and headers from different installations.
    """

    verilator_path = pathlib.Path(verilator_cmd)
    if verilator_path.is_file():
        env = os.environ.copy()
        env["PATH"] = str(verilator_path.parent) + os.pathsep + env.get("PATH", "")
        return env
    return None


def parse_explicit_cycle_count(path: pathlib.Path) -> int | None:
    """Return an explicit cycle/latency count from a LegUp report if one exists.

    Reports vary by pass and LegUp configuration, so the parser intentionally
    accepts only lines that spell out cycle/latency counts.  It avoids deriving a
    fake total from loop IIs because that would hide dynamic trip-count effects.
    """
    if not path.exists():
        return None
    patterns = [
        r"\b(?:total\s+)?(?:cycle|cycles|latency)\b[^0-9]*(?P<value>[0-9,]+)",
        r"\b(?P<value>[0-9,]+)\s+(?:cycle|cycles)\b",
    ]
    for raw in path.read_text(errors="ignore").splitlines():
        line = raw.strip()
        for pattern in patterns:
            m = re.search(pattern, line, re.IGNORECASE)
            if m:
                return _to_int(m.group("value"))
    return None


def parse_fsm_state_count(verilog_path: pathlib.Path | None) -> int | None:
    """Count generated LegUp FSM state constants in the Verilog output.

    The count is a structural schedule-size metric, not a runtime latency.  It is
    useful when LegUp does not emit an explicit total cycle count and maps well to
    the scheduler's finite-state-machine output.
    """
    if verilog_path is None or not verilog_path.exists():
        return None
    text = verilog_path.read_text(errors="ignore")
    states: set[str] = set()
    state_re = re.compile(r"\b(LEGUP_[A-Za-z0-9_]+)\b")
    for line in text.splitlines():
        for name in state_re.findall(line):
            if name != "LEGUP_0":
                states.add(name)
    if "LEGUP_0" in text:
        states.add("LEGUP_0")
    return len(states) if states else None


def parse_legup_resources(path: pathlib.Path | None) -> dict:
    """Extract LegUp-native resource estimates from the run log.

    Role:
    Parse the textual LegUp transcript for frequency, logic, register, DSP, and
    operation-usage estimates produced before optional Vivado synthesis.

    Goal:
    Keep tool-estimate data available in both the detailed native report and the
    normalized cross-tool QoR report without inventing values when the log is
    missing.
    """

    out: dict = {
        "estimated_fmax_mhz": None,
        "logic_elements": None,
        "combinational": None,
        "registers": None,
        "dsp_elements": None,
        "critical_path_op": None,
        "operation_usage": {},
    }
    if path is None or not path.exists():
        return out
    for raw in path.read_text(errors="ignore").splitlines():
        line = raw.strip()
        if not line:
            continue
        m = re.search(r"Fmax:\s*([0-9.]+)\s*MHz", line)
        if m:
            out["estimated_fmax_mhz"] = float(m.group(1))
            continue
        m = re.search(r"Logic Elements:\s*([0-9,]+)", line)
        if m:
            out["logic_elements"] = _to_int(m.group(1))
            continue
        m = re.search(r"Combinational:\s*([0-9,]+)", line)
        if m:
            out["combinational"] = _to_int(m.group(1))
            continue
        m = re.search(r"Registers:\s*([0-9,]+)", line)
        if m:
            out["registers"] = _to_int(m.group(1))
            continue
        m = re.search(r"DSP Elements:\s*([0-9,]+)", line)
        if m:
            out["dsp_elements"] = _to_int(m.group(1))
            continue
        m = re.search(r'Critical path contains operation:\s*(.+)$', line)
        if m:
            out["critical_path_op"] = m.group(1)
            continue
        m = re.search(r'Operation "([^"]+)" x\s*([0-9,]+)', line)
        if m:
            out["operation_usage"][m.group(1)] = _to_int(m.group(2))
    return out


def parse_vivado_reports(vivado_dir: pathlib.Path) -> dict | None:
    util = vivado_dir / "util.rpt"
    timing = vivado_dir / "timing.rpt"
    if not util.exists() and not timing.exists():
        return None

    out: dict = {
        "lut": None,
        "ff": None,
        "bram": None,
        "dsp": None,
        "tclk_ns": None,
        "slack_ns": None,
    }

    if util.exists():
        txt = util.read_text(errors="ignore")
        for name, key in (
            (r"CLB LUTs|Slice LUTs|LUTs", "lut"),
            (r"CLB Registers|Slice Registers|Registers", "ff"),
            (r"Block RAM Tile|BRAM|RAMB", "bram"),
            (r"DSPs", "dsp"),
        ):
            val = _extract_vivado_util(txt, name)
            if val is not None:
                out[key] = val

    if timing.exists():
        txt = timing.read_text(errors="ignore")
        m = re.search(r"Data Path Delay:\s*([0-9.]+)ns", txt)
        if m:
            out["tclk_ns"] = float(m.group(1))
        m = re.search(r"Slack.*?:\s*([-0-9.]+)ns", txt)
        if m:
            out["slack_ns"] = float(m.group(1))

    return out


def _extract_vivado_util(text: str, pattern: str) -> int | None:
    for line in text.splitlines():
        if not re.search(pattern, line):
            continue
        cells = [c.strip().replace(",", "") for c in line.split("|")]
        nums = [c for c in cells if re.fullmatch(r"-?\d+(?:\.\d+)?", c)]
        if nums:
            try:
                return int(float(nums[0]))
            except ValueError:
                return None
    return None


def _to_int(value) -> int | None:
    if value is None:
        return None
    try:
        return int(str(value).replace(",", "").strip())
    except ValueError:
        return None


def benchmark_record(
    bench: str,
    benchdir: pathlib.Path,
    sources: SourceSet | None,
    prepdir: pathlib.Path | None,
    builddir: pathlib.Path | None,
    synth_source: pathlib.Path | None,
    log_path: pathlib.Path | None,
    returncode: int | None,
    run_vivado: bool,
) -> dict:
    record: dict = {
        "benchmark": bench,
        "status": "failed" if returncode not in (None, 0) else "ok",
        "returncode": returncode,
        "paths": {
            "benchmark_dir": str(benchdir),
            "work_src": str(prepdir) if prepdir else None,
            "work_build": str(builddir) if builddir else None,
            "synth_source": str(synth_source) if synth_source else None,
            "log": str(log_path) if log_path else None,
            "verilog": str(builddir / f"{synth_source.stem}.v") if builddir and synth_source else None,
        },
        "sources": asdict(sources) if sources else None,
        "pipelined_loops": [],
        "schedule": {},
        "legup": parse_legup_resources(log_path),
        "vivado": None,
        "simulation": None,
        "error": None,
    }

    if builddir is None:
        record["error"] = "benchmark staging failed"
        return record

    verilog_path = builddir / f"{synth_source.stem}.v" if synth_source else None
    pipelining_rpt = builddir / "pipelining.rtl.legup.rpt"
    if pipelining_rpt.exists():
        record["pipelined_loops"] = parse_pipeline_report(pipelining_rpt)
    record["schedule"] = parse_schedule_info(builddir, verilog_path, record["pipelined_loops"])

    if run_vivado:
        record["vivado"] = parse_vivado_reports(builddir / "vivado")

    if returncode not in (None, 0):
        record["status"] = "failed"
        if log_path and log_path.exists():
            record["error"] = _extract_last_error(log_path)
    return record


def _extract_last_error(log_path: pathlib.Path) -> str | None:
    lines = log_path.read_text(errors="ignore").splitlines()
    interesting = [ln for ln in lines if re.search(r"\b(error|ERROR|failed|FAILURE)\b", ln)]
    return interesting[-1] if interesting else (lines[-1] if lines else None)


def write_ascii_table(path: pathlib.Path, payload: dict) -> None:
    """Write a fixed-width QoR/schedule table next to the JSON results.

    The table is intentionally derived only from the JSON payload so it remains a
    presentation layer: scripts should consume JSON, while humans can inspect the
    table quickly in logs or terminals.
    """
    rows = []
    for name, rec in (payload.get("benchmarks") or {}).items():
        viv = rec.get("vivado") or {}
        sched = rec.get("schedule") or {}
        err = " ".join((rec.get("error") or "").split())[:42]
        rows.append(
            {
                "benchmark": name,
                "status": rec.get("status", ""),
                "top": ((rec.get("sources") or {}).get("top_function") or ""),
                "states": sched.get("fsm_states"),
                "cycles": sched.get("cycle_count_static"),
                "loops": sched.get("pipelined_loop_count"),
                "ii_min": sched.get("min_ii"),
                "ii_max": sched.get("max_ii"),
                "depth_min": sched.get("min_depth_cycles"),
                "depth_max": sched.get("max_depth_cycles"),
                "LUT": viv.get("lut"),
                "FF": viv.get("ff"),
                "DSP": viv.get("dsp"),
                "BRAM": viv.get("bram"),
                "Tclk": viv.get("tclk_ns"),
                "Slack": viv.get("slack_ns"),
                "err": err,
            }
        )
    columns = [
        ("benchmark", 24),
        ("status", 7),
        ("top", 20),
        ("states", 6),
        ("cycles", 7),
        ("loops", 5),
        ("ii_min", 6),
        ("ii_max", 6),
        ("depth_min", 9),
        ("depth_max", 9),
        ("LUT", 8),
        ("FF", 8),
        ("DSP", 5),
        ("BRAM", 5),
        ("Tclk", 7),
        ("Slack", 7),
        ("err", 42),
    ]
    lines = []
    lines.append(" | ".join(name.ljust(width) for name, width in columns))
    lines.append("-+-".join("-" * width for _, width in columns))
    for row in rows:
        lines.append(" | ".join(_table_cell(row.get(name), width) for name, width in columns))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def _table_cell(value, width: int) -> str:
    if value is None:
        text = "-"
    elif isinstance(value, float):
        text = f"{value:.3f}"
    else:
        text = str(value)
    return text[:width].ljust(width)


def write_normalized_qor(path: pathlib.Path, payload: dict, part: str, period_ns: float) -> None:
    """Write the common cross-tool QoR report for a LegUp batch.

    Role:
    Convert the existing LegUp-native JSON payload into the shared benchmark QoR
    schema used by the Bambu runner and future tool adapters.

    Goal:
    Preserve the detailed LegUp report unchanged while adding a compact artifact
    whose fields line up with Bambu/PANDA and Dynamatic comparisons.
    """

    records = [
        common_qor.normalize_legup_record(record, part=part, period_ns=period_ns)
        for _name, record in sorted((payload.get("benchmarks") or {}).items())
    ]
    normalized = common_qor.build_report(
        tool="legup",
        config={
            **(payload.get("config") or {}),
            "normalized_output": str(path),
        },
        benchmarks=records,
    )
    common_qor.write_json(path, normalized)


def main() -> int:
    ap = argparse.ArgumentParser(description="Run LegUp over all OpenHLS benchmarks and emit JSON results")
    ap.add_argument("--openhls-root", type=pathlib.Path, default=DEFAULT_OPENHLS_ROOT)
    ap.add_argument("--work-root", type=pathlib.Path, default=DEFAULT_WORK_ROOT)
    ap.add_argument("--output", type=pathlib.Path, default=DEFAULT_WORK_ROOT / "legup_run_all.json")
    ap.add_argument(
        "--normalized-output",
        type=pathlib.Path,
        default=DEFAULT_NORMALIZED_OUTPUT,
        help="common cross-tool QoR JSON output path",
    )
    ap.add_argument("--bench", action="append", help="Limit to a benchmark name; may be repeated")
    ap.add_argument("--part", default=os.environ.get("VIVADO_PART", "xc7a100tcsg324-1"))
    ap.add_argument("--period", type=float, default=float(os.environ.get("CLOCK_PERIOD", "10.0")))
    ap.add_argument("--image", default=os.environ.get("LEGUP_IMAGE", "legup14:latest"))
    ap.add_argument(
        "--legup-script",
        type=pathlib.Path,
        default=REPO_ROOT / "hls_bench" / "scripts" / "legup_vivado_flow.sh",
    )
    ap.add_argument("--vivado", default=os.environ.get("VIVADO", "vivado"))
    ap.add_argument("--verilator", default=os.environ.get("VERILATOR", str(pathlib.Path.home() / "oss-cad-suite" / "bin" / "verilator")))
    ap.add_argument("--run-vivado", action="store_true", help="Run Vivado post-synthesis reports instead of default skip-vivado")
    ap.add_argument("--run-simulation", action="store_true", help="Run generated RTL with Verilator and record exact cycles")
    ap.add_argument("--include-cxx", action="store_true", help="Attempt benchmarks whose wrapper/kernel are C++")
    ap.add_argument("--table-output", type=pathlib.Path, help="Also write a fixed-width ASCII table summary")
    args = ap.parse_args()

    benches = args.bench or list_benchmarks(args.openhls_root)
    results: dict[str, dict] = {}
    args.work_root.mkdir(parents=True, exist_ok=True)

    for bench in benches:
        benchdir = args.openhls_root / bench
        sources = None
        prepdir = builddir = combined = log_path = None
        returncode: int | None = None

        if not benchdir.is_dir():
            results[bench] = {
                "benchmark": bench,
                "status": "skipped",
                "returncode": None,
                "error": f"benchmark directory not found: {benchdir}",
                "paths": {"benchmark_dir": str(benchdir)},
            }
            continue

        try:
            sources = detect_sources(benchdir)
            if sources.wrapper is None or sources.kernel is None:
                raise FileNotFoundError(f"no wrapper/kernel source found for {bench}")
            if sources.cxx_mode and not args.include_cxx:
                results[bench] = {
                    "benchmark": bench,
                    "status": "skipped",
                    "returncode": None,
                    "error": "C++ benchmark skipped by default; rerun with --include-cxx",
                    "paths": {"benchmark_dir": str(benchdir)},
                    "sources": asdict(sources),
                }
                continue

            sources, prepdir, builddir, combined = stage_benchmark(REPO_ROOT, args.openhls_root, args.work_root, bench)
            returncode, cmd, log_path = run_legup_flow(
                REPO_ROOT,
                combined,
                builddir,
                args.part,
                args.period,
                args.image,
                args.legup_script,
                args.run_vivado,
                sources.cxx_mode,
                args.vivado,
                sources.top_function or "main",
                ["LEGUP"] if bench == "rv32i_truecomet" else [],
            )
            record = benchmark_record(bench, benchdir, sources, prepdir, builddir, combined, log_path, returncode, args.run_vivado)
            record["command"] = cmd
            if args.run_simulation and returncode == 0:
                record["simulation"] = run_legup_cycle_simulation(builddir, combined, args.verilator, bench)
                if record["simulation"]["status"] != "ok":
                    # Synthesis alone cannot validate an RTL result or cycle count.
                    record["status"] = "failed"
                    record["error"] = record["simulation"]["message"]
            results[bench] = record
        except subprocess.CalledProcessError as exc:
            record = benchmark_record(bench, benchdir, sources, prepdir, builddir, combined, log_path, exc.returncode, args.run_vivado)
            record["status"] = "failed"
            record["error"] = f"preprocessing failed: {exc}"
            results[bench] = record
        except Exception as exc:
            record = benchmark_record(bench, benchdir, sources, prepdir, builddir, combined, log_path, returncode, args.run_vivado)
            record["status"] = "failed" if record.get("status") != "skipped" else record["status"]
            record["error"] = str(exc)
            results[bench] = record

    summary = {
        "passed": sum(1 for r in results.values() if r.get("status") == "ok"),
        "failed": sum(1 for r in results.values() if r.get("status") == "failed"),
        "skipped": sum(1 for r in results.values() if r.get("status") == "skipped"),
        "total": len(results),
    }

    payload = {
        "generated_at": _dt.datetime.now(tz=_dt.timezone.utc).isoformat(),
        "config": {
            "openhls_root": str(args.openhls_root),
            "work_root": str(args.work_root),
            "output": str(args.output),
            "normalized_output": str(args.normalized_output),
            "table_output": str(args.table_output) if args.table_output else None,
            "part": args.part,
            "period_ns": args.period,
            "image": args.image,
            "legup_script": str(args.legup_script),
            "vivado": args.vivado,
            "verilator": args.verilator,
            "run_vivado": args.run_vivado,
            "run_simulation": args.run_simulation,
            "default_skip_vivado": not args.run_vivado,
            "include_cxx": args.include_cxx,
        },
        "summary": summary,
        "benchmarks": results,
    }
    write_json(args.output, payload)
    write_normalized_qor(args.normalized_output, payload, args.part, args.period)
    if args.table_output:
        write_ascii_table(args.table_output, payload)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["failed"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
