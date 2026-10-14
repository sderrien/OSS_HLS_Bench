"""Regression coverage for OpenHLS benchmark DSE generation.

Role:
Validate the runner-owned DSE manifest before it reaches OpenHLS.

Goal:
Keep every pointer argument on the fixed-latency external-BRAM interface used
by the cross-tool benchmark baseline, rather than silently reverting to the
tagged external-bus protocol.
"""

from __future__ import annotations

from hls_bench import inputs, loop_labels, openhls_runner


def test_build_benchmark_dse_uses_one_port_external_bram(monkeypatch, tmp_path) -> None:
    """Build BRAM contracts for every discovered pointer argument.

    Role:
    Replace Clang AST discovery with deterministic argument names so this unit
    test isolates the runner's manifest construction.

    Goal:
    Verify that OpenHLS receives a fixed one-cycle, one-read-port, one-write-port
    interface without legacy local-RAM resource directives.
    """
    monkeypatch.setattr(
        openhls_runner,
        "discover_array_arguments",
        lambda *_args: ["A", "B"],
    )
    loops = [
        loop_labels.LoopLabel(
            line_index=0,
            label="L_1_0",
            label_line_index=0,
            nest_id=1,
            nest_depth=0,
            kind="for",
            already_labeled=True,
        )
    ]

    dse = openhls_runner.build_benchmark_dse(
        tmp_path / "kernel.c",
        "dotprod",
        loops,
        "/unused/llvm/bin",
        tmp_path,
        openhls_runner.DEFAULT_OPENHLS_ROOT,
    )

    assert dse["interfaces"] == [
        {
            "function": "dotprod",
            "argument": "A",
            "kind": "external_bram",
            "latency": 1,
            "read_ports": 1,
            "write_ports": 1,
        },
        {
            "function": "dotprod",
            "argument": "B",
            "kind": "external_bram",
            "latency": 1,
            "read_ports": 1,
            "write_ports": 1,
        },
    ]
    assert dse["resources"] == []


def test_build_benchmark_dse_pipelines_only_innermost_loop_per_nest(monkeypatch, tmp_path) -> None:
    """Exclude unsupported outer-loop pipeline intent from a nested loop nest.

    Role:
    Model the deterministic labels emitted for a nested kernel such as SPMV,
    including a separate top-level loop and two sibling leaf loops.

    Goal:
    Ensure runner-generated DSE asks OpenHLS to pipeline only leaf loops, which
    are eligible for its single-block hyperblock conversion.
    """
    monkeypatch.setattr(openhls_runner, "discover_array_arguments", lambda *_args: [])
    loops = [
        loop_labels.LoopLabel(0, "L_1_0", 0, 1, 0, "for", True),
        loop_labels.LoopLabel(1, "L_1_1", 1, 1, 1, "for", True),
        loop_labels.LoopLabel(2, "L_1_2", 2, 1, 1, "for", True),
        loop_labels.LoopLabel(3, "L_2_0", 3, 2, 0, "for", True),
    ]

    dse = openhls_runner.build_benchmark_dse(
        tmp_path / "kernel.c",
        "spmv",
        loops,
        "/unused/llvm/bin",
        tmp_path,
        openhls_runner.DEFAULT_OPENHLS_ROOT,
    )

    assert [entry["selector"]["label"] for entry in dse["loops"]] == [
        "L_1_1",
        "L_1_2",
        "L_2_0",
    ]


def test_generated_openhls_harness_matches_const_array_abis() -> None:
    """Preserve canonical const qualifiers in generated golden-driver calls.

    Role:
    Render the shared-input harnesses used by the OpenHLS golden-output flow.

    Goal:
    Prevent C conflicting-declaration errors when a kernel header declares
    read-only array parameters as const-qualified pointers.
    """
    matmul = inputs.generate_c_harness(
        inputs.get_benchmark_inputs("matmul"),
        use_dynamatic_call=False,
        write_output_file=True,
    )
    passthrough = inputs.generate_c_harness(
        inputs.get_benchmark_inputs("passthrough"),
        use_dynamatic_call=False,
        write_output_file=True,
    )

    assert "void matmul(const int A[64][64], const int B[64][64], int C[64][64]);" in matmul
    assert "void passthrough(const int data_in[1024], int data_out[1024]);" in passthrough
