"""Resolve user-configurable toolchain locations for the benchmark flows.

Role:
    Keep executable and external-project discovery in one module instead of
    embedding one developer's home directory in every runner.

Goal:
    Make local runs portable while preserving the existing environment-variable
    overrides and allowing commands installed on ``PATH`` to work directly.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil


def _env_path(name: str, default: Path) -> Path:
    """Return an expanded path from an environment variable or a default."""

    return Path(os.environ.get(name, str(default))).expanduser()


def _env_command(name: str, default: str) -> str:
    """Return an executable override, PATH lookup, or a descriptive command."""

    configured = os.environ.get(name)
    if configured:
        return str(Path(configured).expanduser())
    return shutil.which(default) or default


OPENHLS_ROOT = _env_path("OPENHLS_ROOT", Path.home() / "OpenHLS")
OPENHLS_LLVM_BIN = _env_path("VIBEHLS_LLVM_BIN", Path.home() / ".local" / "build" / "llvm22-project" / "bin")
OPENHLS_CIRCT_BIN = _env_path("VIBEHLS_CIRCT_BIN", OPENHLS_LLVM_BIN)

VHLS_CLI = os.environ.get("VHLS_CLI", str(OPENHLS_ROOT / "tools" / "bin" / "ohls-cli"))
VIVADO = _env_command("VIVADO", "vivado")
VITIS_HLS = _env_command("VITIS_HLS", "vitis_hls")
BAMBU = _env_command("BAMBU", "bambu")
