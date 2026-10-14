"""Repository paths shared by benchmark runners and report generation.

Role:
Keep benchmark inputs, tool work trees, run logs, and published reports in named
locations instead of repeating ``osshls_bench``-specific paths across runners.

Goal:
Make the repository reorganization incremental: existing benchmark sources remain
usable by default, while users can relocate them with environment variables.
"""

from __future__ import annotations

import os
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
BENCHMARKS_ROOT = Path(os.environ.get("HLS_BENCHMARKS_ROOT", REPO_ROOT / "osshls_bench" / "kernels"))
ARTIFACTS_ROOT = Path(os.environ.get("HLS_ARTIFACTS_ROOT", REPO_ROOT / "artifacts"))
RUNS_ROOT = ARTIFACTS_ROOT / "runs"
REPORTS_ROOT = ARTIFACTS_ROOT / "reports"
TOOL_WORK_ROOT = ARTIFACTS_ROOT / "tool-work"

LEGUP_WORK_ROOT = TOOL_WORK_ROOT / "legup"
BAMBU_WORK_ROOT = TOOL_WORK_ROOT / "bambu"
DYNAMATIC_WORK_ROOT = TOOL_WORK_ROOT / "dynamatic"
VITIS_WORK_ROOT = TOOL_WORK_ROOT / "vitis"
OPENHLS_WORK_ROOT = TOOL_WORK_ROOT / "openhls"
