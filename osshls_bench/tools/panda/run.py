#!/usr/bin/env python3
"""Wrapper for the legacy Bambu/PANDA benchmark runner."""

from __future__ import annotations

from pathlib import Path
import runpy
import sys


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))

runpy.run_path(str(REPO_ROOT / "osshls_bench" / "tools" / "panda" / "bambu-run-all.py"), run_name="__main__")
