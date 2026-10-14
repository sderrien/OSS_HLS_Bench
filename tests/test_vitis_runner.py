"""Regression coverage for Vitis external-memory directives.

Role:
Exercise Vitis Tcl generation without invoking Vitis HLS.

Goal:
Keep Vitis array interfaces constrained to the same single-port BRAM contract
used by OpenHLS, preventing inferred extra ports from distorting comparisons.
"""

from __future__ import annotations

from types import SimpleNamespace

from hls_bench import vitis_runner


def test_qr_givens_arrays_use_one_port_ap_memory() -> None:
    """Emit one shared read/write BRAM port for both QR matrix arguments.

    Role:
    Model the subset of a Vitis job consumed by the interface-directive helper.

    Goal:
    Prevent Vitis from inferring its default multiple read ports for ``a`` and
    ``r``, which would make its QR rotation II incomparable with OpenHLS.
    """

    job = SimpleNamespace(name="qr_givens", top_function="qr_givens")

    assert vitis_runner.vitis_interface_directives(job) == [
        "set_directive_interface -mode ap_memory -storage_type ram_1p {qr_givens} a",
        "set_directive_interface -mode ap_memory -storage_type ram_1p {qr_givens} r",
    ]


def test_rv32i_one_port_memories_preserve_explicit_depth() -> None:
    """Keep RV32I's pointer-memory bounds while constraining its port count."""

    job = SimpleNamespace(name="rv32i", top_function="cpu_run")

    directives = vitis_runner.vitis_interface_directives(job)

    assert len(directives) == 5
    assert all("-storage_type ram_1p" in directive for directive in directives)
    assert all(f"-depth {vitis_runner.RV32I_MEMORY_ELEMENTS}" in directive for directive in directives)


def test_truecomet_vitis_kernel_is_compiled_with_explicit_memory_path() -> None:
    """Pass VITIS to APCC for TrueComet's staged kernel.

    Role:
    Inspect the focused source compile flags independently of Vitis HLS.

    Goal:
    Prevent a runner regression that leaves the source's Vitis byte-access path
    inactive while the benchmark appears to have tool-specific source support.
    """

    job = SimpleNamespace(name="rv32i_truecomet")

    assert vitis_runner.vitis_kernel_cflags(job) == "-DVITIS"
