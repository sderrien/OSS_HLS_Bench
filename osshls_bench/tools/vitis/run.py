#!/usr/bin/env python3
"""Wrapper for the local Vitis HLS benchmark CLI."""

from __future__ import annotations

from pathlib import Path
import runpy
import sys


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))

runpy.run_module("hls_bench.cli", run_name="__main__")
