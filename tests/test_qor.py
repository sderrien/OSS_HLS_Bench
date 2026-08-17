"""Regression coverage for post-synthesis QoR report parsing.

Role:
Validate that the common Vivado report parser selects top-level utilization
rows rather than similarly named detail rows.

Goal:
Keep generated comparison tables faithful to raw Vivado utilization, including
when a LUT detail row containing the word ``Register`` precedes the actual
``Slice Registers`` total.
"""

from __future__ import annotations

from hls_bench import parallel_runner
from hls_bench.qor import parse_vivado_report_files


def test_vivado_util_parser_ignores_lut_as_shift_register_detail(tmp_path) -> None:
    """Read the top-level register total from a representative Vivado table.

    Role:
    Reproduce the ordering in the OpenHLS SPMV utilization report, where the
    ``LUT as Shift Register`` detail row appears immediately before the
    top-level ``Slice Registers`` row.

    Goal:
    Prevent the generic ``Registers`` matcher from reporting the two LUT shift
    registers as the entire design's flip-flop count.
    """
    util_path = tmp_path / "util.rpt"
    timing_path = tmp_path / "timing.rpt"
    util_path.write_text(
        "| LUT as Shift Register  |    2 |     0 |\n"
        "| Slice Registers        |  839 |     0 |\n",
        encoding="utf-8",
    )
    timing_path.write_text("", encoding="utf-8")

    metrics = parse_vivado_report_files(util_path, timing_path)

    assert metrics["ff"] == 839


def test_vitis_loop_ii_supplements_top_report_with_pipeline_submodule(monkeypatch, tmp_path) -> None:
    """Read an II reported only by Vitis's dedicated pipeline XML.

    Role:
    Reproduce Vitis's ``qr_givens`` reporting split, where the top-level csynth
    report lists ``L_2_2`` but omits its pipeline metrics while the generated
    pipeline-submodule report contains the II and depth.

    Goal:
    Prevent the QoR table from showing ``-`` for a Vitis loop that has an
    explicit pipeline II in an auxiliary synthesis report.
    """

    report_dir = tmp_path / "qr_givens" / "project" / "solution1" / "syn" / "report"
    report_dir.mkdir(parents=True)
    (report_dir / "qr_givens_csynth.xml").write_text(
        "<profile><PerformanceEstimates><SummaryOfLoopLatency>"
        "<L_2_2><Latency>112</Latency></L_2_2>"
        "</SummaryOfLoopLatency></PerformanceEstimates></profile>",
        encoding="utf-8",
    )
    (report_dir / "qr_givens_Pipeline_L_2_2_csynth.xml").write_text(
        "<profile><PerformanceEstimates><SummaryOfLoopLatency>"
        "<L_2_2><PipelineII>14</PipelineII><PipelineDepth>14</PipelineDepth></L_2_2>"
        "</SummaryOfLoopLatency></PerformanceEstimates></profile>",
        encoding="utf-8",
    )
    monkeypatch.setattr(parallel_runner.paths, "VITIS_WORK_ROOT", tmp_path)

    loops = parallel_runner.parse_vitis_loop_ii("qr_givens")

    assert loops == {"L_2_2": {"ii": 14, "depth": 14, "source": "L_2_2"}}
