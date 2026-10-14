from hls_bench.loop_labels import label_c_source_text


def test_label_precedes_attached_hls_pragma() -> None:
    source = "void f(void) {\n  #pragma HLS pipeline\n  for (int i = 0; i < 4; ++i) {}\n}\n"

    labeled, entries = label_c_source_text(source)

    assert entries[0].label == "L_1_0"
    assert "  L_1_0:\n  #pragma HLS pipeline\n  for" in labeled
