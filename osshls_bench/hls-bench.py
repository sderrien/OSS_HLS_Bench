#!/usr/bin/env python3
"""Wrapper entry point for the reorganized OSS HLS benchmark tree."""

from __future__ import annotations

from pathlib import Path
import runpy
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

runpy.run_module("hls_bench.cli", run_name="__main__")
