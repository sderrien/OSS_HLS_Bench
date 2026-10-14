"""Regression coverage for Bambu runner interface selection.

Role:
Load the standalone Bambu runner and inspect its generated testbench command.

Goal:
Keep TrueComet's byte-addressed data-memory ABI on Bambu's minimal interface,
which is required for legal byte and halfword operations to reach RTL simulation.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

from hls_bench import inputs


REPO_ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = REPO_ROOT / "osshls_bench" / "tools" / "panda" / "bambu-run-all.py"


def _load_bambu_runner():
    """Load the hyphenated standalone runner as a testable Python module.

    Role:
    Register the dynamically loaded module before executing it so dataclass
    annotations resolve exactly as they do when the runner is invoked normally.

    Goal:
    Test command construction without requiring a Bambu installation or running
    an RTL simulation in the unit-test suite.
    """

    spec = importlib.util.spec_from_file_location("bambu_run_all_test", RUNNER_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_truecomet_bambu_command_keeps_byte_addressed_memory_unaligned() -> None:
    """Select the wrapper that permits TrueComet's byte and halfword accesses.

    Role:
    Construct the focused Bambu simulation command without launching external
    tools.

    Goal:
    Prevent a regression to the inferred interface, whose word-alignment policy
    rejects the benchmark's required byte-addressed data-memory operations.
    """

    runner = _load_bambu_runner()
    job = runner.BenchmarkJob(
        name="rv32i_truecomet",
        directory=REPO_ROOT / "osshls_bench" / "kernels" / "rv32i_truecomet",
        source=REPO_ROOT / "osshls_bench" / "kernels" / "rv32i_truecomet" / "src" / "rv32i_true_comet.c",
        top_function="rv32i_true_comet_run",
    )

    command = runner._build_bambu_simulation_command(
        bambu="bambu",
        job=job,
        translated_source=job.source,
        testbench_source=job.source,
        output_dirname="bambu_test",
        extra_args=[],
    )

    assert "--generate-interface=MINIMAL" in command
    assert "-DBAMBU" in runner._benchmark_extra_bambu_args(job)
    fixture = inputs.get_benchmark_inputs("rv32i_truecomet")
    assert fixture is not None
    assert [(arg.name, arg.shape) for arg in fixture.args] == [
        ("instr_mem", (64,)),
        ("data_mem", (256,)),
    ]
