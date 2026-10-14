"""Native equivalence checks for TrueComet byte-memory compiler variants.

Role:
Compile the canonical local TrueComet kernel together with its directed C++
testbench for every supported tool definition and the portable fallback.

Goal:
Prove that the distinct HLS-oriented memory lowerings preserve the exact RV32I
byte, halfword, word, control-flow, and terminal-status behavior before tool
flows consume the source.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = REPO_ROOT / "osshls_bench" / "kernels" / "rv32i_truecomet" / "src"


@pytest.mark.parametrize("compiler_define", [None, "OPENHLS", "VITIS", "BAMBU", "LEGUP"])
def test_truecomet_memory_variants_match_directed_native_testbench(
    compiler_define: str | None, tmp_path: Path
) -> None:
    """Run all directed TrueComet cases for one memory implementation.

    Role:
    Build the C kernel and existing C++ testbench as one host executable using
    the same preprocessor definition passed by the corresponding HLS runner.

    Goal:
    Detect any semantic drift in byte ordering, sign extension, or stores across
    OpenHLS's packed memcpy path, the Vitis/Bambu/LegUp explicit paths, and the
    portable fallback without substituting reduced synthetic inputs.
    """

    compiler = shutil.which("clang++")
    if compiler is None:
        pytest.skip("clang++ is required for native TrueComet variant coverage")
    executable = tmp_path / "rv32i_truecomet"
    command = [compiler, "-std=c++17", "-O0"]
    if compiler_define is not None:
        command.append(f"-D{compiler_define}")
    command.extend(
        [
            str(SOURCE_DIR / "rv32i_true_comet.c"),
            str(SOURCE_DIR / "main_rv32i_true_comet.cpp"),
            "-o",
            str(executable),
        ]
    )

    build = subprocess.run(command, text=True, capture_output=True, check=False)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(executable)], text=True, capture_output=True, check=False)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "arithmetic_and_memory=yes" in run.stdout
    assert "control_flow=yes" in run.stdout
    assert "terminal_statuses=yes" in run.stdout
    assert "pass=yes" in run.stdout
