"""Describe the local Dynamatic command sequence for benchmark integration.

Role:
Capture how this repository would invoke the local Dynamatic installation while
the LegUp/Bambu common reporting work is being generalized.

Goal:
Provide a safe adapter boundary for Dynamatic execution while allowing this
repository to substitute a local synthesis wrapper for the installed script.

How:
The helpers construct command lines matching Dynamatic's shell frontend scripts:
``compile.sh``, ``write-hdl.sh``, and ``synthesize.sh``.  The functions return
data only; callers remain responsible for choosing when to execute commands.
When ``synthesis_script`` is provided, the generated synthesis command targets
the repo-local wrapper that accepts explicit Vivado part and clock arguments.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class DynamaticFlowConfig:
    """Configuration needed to describe one Dynamatic benchmark flow.

    Role:
    Store the script arguments used by Dynamatic's own interactive frontend when
    it expands `compile`, `write-hdl`, and `synthesize` commands.

    Goal:
    Keep future runner code from rebuilding long positional command lines by
    hand and make all default assumptions visible in one data structure.
    """

    dynamatic_dir: Path
    source_dir: Path
    output_dir: Path
    kernel_name: str
    clock_period_ns: float = 10.0
    synthesis_part: str = "xc7a100tcsg324-1"
    synthesis_script: Path | None = None
    simulation_script: Path | None = None
    use_simple_buffers: bool = False
    buffer_algorithm: str = "on-merges"
    use_sharing: bool = False
    fp_units_generator: str = "flopoco"
    use_vivado_fpu: bool = False
    use_rigidification: bool = False
    k_induction: int = 0
    disable_lsq: bool = False
    fast_token_delivery: bool = False
    milp_solver: str = "cbc"
    straight_to_queue: bool = False
    speculation: bool = False
    enable_short_circuit: bool = False
    hdl: str = "verilog"
    synthesis_part_note: str = "Repo-local synthesis wrapper accepts explicit Vivado part and clock"
    simulation_source_dir: Path | None = None
    vivado_path: Path | None = None
    simulator: str = "verilator"
    simulation_timeout_cycles: int = 0


def detect_script_interface(dynamatic_dir: Path) -> str:
    """Detect which Dynamatic shell-script interface is installed.

    Role:
    Distinguish the legacy v2.0.0 scripts installed under ``~/.local`` from the
    newer source-tree scripts that accept more compile and synthesis arguments.

    Goal:
    Let one adapter support both local installations without guessing positional
    arguments incorrectly and launching a malformed flow.
    """

    compile_script = dynamatic_dir / "tools" / "dynamatic" / "scripts" / "compile.sh"
    text = compile_script.read_text(encoding="utf-8", errors="replace") if compile_script.exists() else ""
    if "USE_SIMPLE_BUFFERS=$5" in text:
        return "legacy-v2"
    return "current"


def build_flow_commands(config: DynamaticFlowConfig) -> dict[str, list[str]]:
    """Build, but do not execute, the Dynamatic compile/HDL/synthesis commands.

    Role:
    Translate a typed adapter configuration into the exact positional arguments
    expected by the local Dynamatic shell scripts.

    Goal:
    Let a future runner share discovery, logging, and QoR reporting code with
    LegUp/Bambu while keeping Dynamatic execution opt-in and reviewable.
    """

    scripts_dir = config.dynamatic_dir / "tools" / "dynamatic" / "scripts"
    interface = detect_script_interface(config.dynamatic_dir)
    synthesis_script = config.synthesis_script or scripts_dir / "synthesize.sh"
    simulation_script = config.simulation_script or scripts_dir / "simulate.sh"
    simulation_source_dir = config.simulation_source_dir or config.source_dir
    vivado_path = config.vivado_path or Path("")
    local_synthesis_cmd = [
        str(synthesis_script),
        str(config.dynamatic_dir),
        str(config.output_dir),
        config.kernel_name,
        config.synthesis_part,
        _format_clock(config.clock_period_ns),
        _format_clock(config.clock_period_ns / 2.0),
        str(vivado_path),
        "true" if config.use_vivado_fpu else "false",
        config.hdl,
    ]
    if interface == "legacy-v2":
        compile_cmd = [
            str(scripts_dir / "compile.sh"),
            str(config.dynamatic_dir),
            str(config.source_dir),
            str(config.output_dir),
            config.kernel_name,
            _flag(config.use_simple_buffers),
        ]
        write_hdl_cmd = [
            str(scripts_dir / "write-hdl.sh"),
            str(config.dynamatic_dir),
            str(config.output_dir),
            config.kernel_name,
        ]
        synthesize_cmd = (
            local_synthesis_cmd
            if config.synthesis_script
            else [
                str(synthesis_script),
                str(config.dynamatic_dir),
                str(config.output_dir),
                config.kernel_name,
            ]
        )
    else:
        compile_cmd = [
            str(scripts_dir / "compile.sh"),
            str(config.dynamatic_dir),
            str(config.source_dir),
            str(config.output_dir),
            config.kernel_name,
            config.buffer_algorithm,
            _format_clock(config.clock_period_ns),
            _flag(config.use_sharing),
            config.fp_units_generator,
            _flag(config.use_rigidification),
            str(config.k_induction),
            _flag(config.disable_lsq),
            _flag(config.fast_token_delivery),
            config.milp_solver,
            _flag(config.straight_to_queue),
            _flag(config.speculation),
            _flag(config.enable_short_circuit),
        ]
        write_hdl_cmd = [
            str(scripts_dir / "write-hdl.sh"),
            str(config.dynamatic_dir),
            str(config.output_dir),
            config.kernel_name,
            config.hdl,
        ]
        synthesize_cmd = (
            local_synthesis_cmd
            if config.synthesis_script
            else [
                str(synthesis_script),
                str(config.dynamatic_dir),
                str(config.output_dir),
                config.kernel_name,
                _format_clock(config.clock_period_ns),
                _format_clock(config.clock_period_ns / 2.0),
            ]
        )
    simulate_cmd = [
        str(simulation_script),
        str(config.dynamatic_dir),
        str(simulation_source_dir),
        str(config.output_dir),
        config.kernel_name,
        str(vivado_path),
        "true" if config.use_vivado_fpu else "false",
        config.simulator,
        config.hdl,
        str(config.simulation_timeout_cycles),
    ]
    return {
        "compile": compile_cmd,
        "write_hdl": write_hdl_cmd,
        "simulate": simulate_cmd,
        "synthesize": synthesize_cmd,
    }


def describe_flow(config: DynamaticFlowConfig) -> dict[str, Any]:
    """Return a JSON-friendly description of the prospective Dynamatic flow.

    Role:
    Package the generated command lines together with notes about which synthesis
    wrapper and part-selection policy are in use.

    Goal:
    Make the integration status explicit in common-framework artifacts and avoid
    accidentally comparing results from different Vivado target parts.
    """

    return {
        "tool": "dynamatic",
        "status": "adapter_only",
        "interface": detect_script_interface(config.dynamatic_dir),
        "commands": build_flow_commands(config),
        "notes": [config.synthesis_part_note],
    }


def _flag(enabled: bool) -> str:
    """Encode a boolean option as Dynamatic's script-level integer flag.

    Role:
    Match the positional argument convention used by Dynamatic's shell scripts.

    Goal:
    Keep command construction readable and avoid repeated string conversions.
    """

    return "1" if enabled else "0"


def _format_clock(value: float) -> str:
    """Format a clock value the same way Dynamatic's frontend does.

    Role:
    Produce three decimal places for full and half clock periods passed to the
    compile and synthesis scripts.

    Goal:
    Keep generated adapter commands aligned with the interactive Dynamatic CLI.
    """

    return f"{value:.3f}"
