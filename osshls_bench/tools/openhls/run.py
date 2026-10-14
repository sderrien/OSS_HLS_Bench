#!/usr/bin/env python3
"""Wrapper for the local OpenHLS/VibeHLS benchmark CLI.

Role:
Expose the OpenHLS runner from the reorganized ``osshls_bench/tools`` tree while
delegating implementation to the ``hls_bench.cli`` module.

Goal:
Keep per-tool launch paths consistent across LegUp, PANDA/Bambu, Dynamatic,
Vitis, and OpenHLS without duplicating argument parsing or run logic.
"""

from __future__ import annotations

from pathlib import Path
import runpy
import sys


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))

runpy.run_module("hls_bench.cli", run_name="__main__")
