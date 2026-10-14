"""Regression coverage for shared deterministic benchmark inputs.

Role:
Validate the tool-independent data fixtures that every benchmark runner stages
into its own tool work directory.

Goal:
Keep the RV32I GCD image synchronized with the canonical embedded program so a
missing instruction cannot silently turn an RTL run into a different workload.
"""

from hls_bench import inputs


def test_rv32i_input_contains_the_complete_gcd_instruction_stream() -> None:
    """Keep the shared fixture aligned with the 21-word canonical GCD program.

    Role:
    Inspect the sparse instruction-memory layout consumed by the RV32I byte-PC
    interpreter.

    Goal:
    Verify that all control-flow instructions through the terminating branch are
    present at byte-PC-derived array indices rather than replaced by zero padding.
    """

    fixture = inputs.get_benchmark_inputs("rv32i")
    assert fixture is not None
    instr_mem = fixture.args[1].values
    expected = (
        0x20002283,
        0x20402303,
        0x00100393,
        0x0062CC63,
        0x02534663,
        0x20502423,
        0x00100493,
        0x20902623,
        0x00000000,
        0x00028413,
        0x00804463,
        0xFE7040E3,
        0xFFF30313,
        0xFFF40413,
        0xFE7048E3,
        0x00030413,
        0x00804463,
        0xFC7044E3,
        0xFFF28293,
        0xFFF40413,
        0xFE7048E3,
    )
    assert tuple(instr_mem[index * 4] for index in range(len(expected))) == expected
