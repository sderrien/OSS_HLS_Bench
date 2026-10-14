#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mnist_cnn.h"
#include "mnist_cnn_model.h"

static uint64_t checksum_logits(const float logits[MNIST_CNN_NUM_CLASSES]);
static uint64_t checksum_logits_bits(const float logits[MNIST_CNN_NUM_CLASSES]);

/*
 * Goal: produce deterministic MNIST-like pixel values in [0, 255].
 * Role in the benchmark: deterministic inputs keep comparisons stable across
 * LLVM, scheduled CycleSim, and RTL verification flows.
 */
static void init_input(float input[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W],
                       uint32_t seed) {
  for (int h = 0; h < MNIST_CNN_INPUT_H; ++h) {
    for (int w = 0; w < MNIST_CNN_INPUT_W; ++w) {
      seed = seed * 1664525u + 1013904223u;
      input[h][w] = (float)((seed >> 24) & 0xFF);
    }
  }
}

/*
 * Goal: execute an independent software reference implementation.
 * Role in the benchmark: this reference path is compared against the kernel
 * output so any functional drift is surfaced as a test failure.
 */
static void mnist_cnn_reference(
    const float input[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W],
    float logits[MNIST_CNN_NUM_CLASSES]) {
  printf("[mnist-ref] begin reference inference\n");
  uint8_t input_q[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W];
  uint8_t conv1_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_CONV1_H][MNIST_CNN_CONV1_W];
  uint8_t pool1_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_POOL1_H][MNIST_CNN_POOL1_W];
  uint8_t conv2_q[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_CONV2_H][MNIST_CNN_CONV2_W];
  float pool2_f[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_POOL2_H][MNIST_CNN_POOL2_W];

  for (int h = 0; h < MNIST_CNN_INPUT_H; ++h) {
    for (int w = 0; w < MNIST_CNN_INPUT_W; ++w) {
      input_q[h][w] = mnist_cnn_quantize_input(input[h][w]);
    }
  }
  {
    uint64_t c = 0;
    for (int h = 0; h < MNIST_CNN_INPUT_H; ++h)
      for (int w = 0; w < MNIST_CNN_INPUT_W; ++w)
        c = (c * 1099511628211ULL) ^ (uint64_t)input_q[h][w];
    printf("[mnist-ref-stage] input_q_checksum=%llu\n", (unsigned long long)c);
  }

  mnist_cnn_conv1_q(input_q, conv1_q);
  {
    uint64_t c = 0;
    for (int oc = 0; oc < MNIST_CNN_CONV1_OUT_C; ++oc)
      for (int h = 0; h < MNIST_CNN_CONV1_H; ++h)
        for (int w = 0; w < MNIST_CNN_CONV1_W; ++w)
          c = (c * 1099511628211ULL) ^ (uint64_t)conv1_q[oc][h][w];
    printf("[mnist-ref-stage] conv1_q_checksum=%llu\n", (unsigned long long)c);
  }

  mnist_cnn_maxpool1_u8(conv1_q, pool1_q);
  {
    uint64_t c = 0;
    for (int oc = 0; oc < MNIST_CNN_CONV1_OUT_C; ++oc)
      for (int h = 0; h < MNIST_CNN_POOL1_H; ++h)
        for (int w = 0; w < MNIST_CNN_POOL1_W; ++w)
          c = (c * 1099511628211ULL) ^ (uint64_t)pool1_q[oc][h][w];
    printf("[mnist-ref-stage] pool1_q_checksum=%llu\n", (unsigned long long)c);
  }

  mnist_cnn_conv2_q(pool1_q, conv2_q);
  {
    uint64_t c = 0;
    for (int oc = 0; oc < MNIST_CNN_CONV2_OUT_C; ++oc)
      for (int h = 0; h < MNIST_CNN_CONV2_H; ++h)
        for (int w = 0; w < MNIST_CNN_CONV2_W; ++w)
          c = (c * 1099511628211ULL) ^ (uint64_t)conv2_q[oc][h][w];
    printf("[mnist-ref-stage] conv2_q_checksum=%llu\n", (unsigned long long)c);
  }

  mnist_cnn_pool2_float(conv2_q, pool2_f);
  {
    int32_t c = 0;
    for (int oc = 0; oc < MNIST_CNN_CONV2_OUT_C; ++oc) {
      for (int h = 0; h < MNIST_CNN_POOL2_H; ++h) {
        for (int w = 0; w < MNIST_CNN_POOL2_W; ++w) {
          const int32_t scaled = (int32_t)(pool2_f[oc][h][w] * 1024.0f);
          c = c * 1315423911 + scaled;
        }
      }
    }
    printf("[mnist-ref-stage] pool2_f_checksum=%d\n", c);
  }

  mnist_cnn_fc_logits(pool2_f, logits);
  {
    int32_t c = 0;
    for (int i = 0; i < MNIST_CNN_NUM_CLASSES; ++i) {
      const int32_t scaled = (int32_t)(logits[i] * 1024.0f);
      c = c * 1315423911 + scaled;
    }
    printf("[mnist-ref-stage] logits_checksum=%d\n", c);
  }
  printf("[mnist-ref] end reference inference\n");
}

/*
 * Goal: create a stable checksum for float output vectors.
 * Role in the benchmark: checksums give a compact, reproducible signature in
 * output logs while still preserving exact mismatch checking via memcmp.
 */
static uint64_t checksum_logits(const float logits[MNIST_CNN_NUM_CLASSES]) {
  uint64_t acc = 0;
  for (int i = 0; i < MNIST_CNN_NUM_CLASSES; ++i) {
    uint32_t bits = 0;
    memcpy(&bits, &logits[i], sizeof(bits));
    acc = (acc * 1315423911ULL) ^ (uint64_t)(bits + 0x9e3779b9u);
  }
  return acc;
}

/*
 * Goal: mirror kernel-side debug checksum over raw logits bits.
 * Role in mismatch debug: provide one directly comparable checksum value
 * between kernel-stage and reference-stage traces.
 */
static uint64_t checksum_logits_bits(const float logits[MNIST_CNN_NUM_CLASSES]) {
  uint64_t acc = 0;
  for (int i = 0; i < MNIST_CNN_NUM_CLASSES; ++i) {
    uint32_t bits = 0;
    memcpy(&bits, &logits[i], sizeof(bits));
    acc = (acc * 1099511628211ULL) ^ (uint64_t)bits;
  }
  return acc;
}

/*
 * Goal: print a compact logits preview for side-by-side debug comparison.
 * Role in mismatch debug: quickly shows whether divergence is global or local.
 */
static void dump_logits_preview(FILE *out, const char *tag,
                                const float logits[MNIST_CNN_NUM_CLASSES]) {
  const int preview = MNIST_CNN_NUM_CLASSES < 8 ? MNIST_CNN_NUM_CLASSES : 8;
  fprintf(out, "[mnist-test] %s logits-preview", tag);
  for (int i = 0; i < preview; ++i) {
    fprintf(out, " c%d=%0.6f", i, logits[i]);
  }
  fprintf(out, "\n");
}

/*
 * Goal: expose raw IEEE-754 payloads for quick ABI/debug triage.
 * Role in mismatch debug: distinguishes numeric drift from bit-pattern
 * corruption at kernel output boundaries.
 */
static void dump_logits_bits_preview(
    FILE *out, const char *tag, const float logits[MNIST_CNN_NUM_CLASSES]) {
  const int preview = MNIST_CNN_NUM_CLASSES < 8 ? MNIST_CNN_NUM_CLASSES : 8;
  fprintf(out, "[mnist-test] %s logits-bits", tag);
  for (int i = 0; i < preview; ++i) {
    uint32_t bits = 0;
    memcpy(&bits, &logits[i], sizeof(bits));
    fprintf(out, " c%d=0x%08x", i, bits);
  }
  fprintf(out, "\n");
}

/*
 * Goal: report first output index where kernel and reference diverge.
 * Role in mismatch debug: gives one precise location/value pair to inspect.
 */
static void report_first_mismatch(
    FILE *out, const float kernel[MNIST_CNN_NUM_CLASSES],
    const float reference[MNIST_CNN_NUM_CLASSES]) {
  for (int i = 0; i < MNIST_CNN_NUM_CLASSES; ++i) {
    if (kernel[i] != reference[i]) {
      const float delta = kernel[i] - reference[i];
      fprintf(out,
              "[mnist-test] first-mismatch index=%d kernel=%0.6f reference=%0.6f delta=%0.6f\n",
              i, kernel[i], reference[i], delta);
      return;
    }
  }
  fprintf(out, "[mnist-test] first-mismatch none\n");
}

int main(int argc, char **argv) {
  if (argc > 1) {
    if (freopen(argv[1], "w", stdout) == NULL) {
      perror("freopen");
      return 1;
    }
  }

  FILE *out = stdout;

  static float input[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W];
  static float logits_kernel[MNIST_CNN_NUM_CLASSES];
  static float logits_ref[MNIST_CNN_NUM_CLASSES];

  init_input(input, 1u);
  fprintf(out, "[mnist-test] begin case=single-seed seed=1\n");

  printf("[mnist-test] begin kernel inference\n");
  mnist_cnn(input, logits_kernel);
  printf("[mnist-test] end kernel inference\n");

  printf("[mnist-test] begin reference inference call\n");
  mnist_cnn_reference(input, logits_ref);
  printf("[mnist-test] end reference inference call\n");

  int mismatch = memcmp(logits_kernel, logits_ref, sizeof(logits_ref));
  uint64_t ksum = checksum_logits(logits_kernel);
  uint64_t rsum = checksum_logits(logits_ref);

  dump_logits_preview(out, "kernel", logits_kernel);
  dump_logits_bits_preview(out, "kernel", logits_kernel);
  dump_logits_preview(out, "reference", logits_ref);
  dump_logits_bits_preview(out, "reference", logits_ref);
  report_first_mismatch(out, logits_kernel, logits_ref);

  fprintf(out, "kernel_checksum=%llu\n", (unsigned long long)ksum);
  fprintf(out, "reference_checksum=%llu\n", (unsigned long long)rsum);
  fprintf(out, "match=%s\n", mismatch == 0 ? "yes" : "no");
  fprintf(out, "[mnist-test] end case=single-seed\n");

  return mismatch == 0 ? 0 : 2;
}
