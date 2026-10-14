#ifndef MNIST_CNN_MODEL_H
#define MNIST_CNN_MODEL_H

#include <stdint.h>

#include "mnist_cnn.h"
#include "mnist_cnn_weights.h"

#define MNIST_CNN_CONV1_H 24
#define MNIST_CNN_CONV1_W 24
#define MNIST_CNN_POOL1_H 12
#define MNIST_CNN_POOL1_W 12
#define MNIST_CNN_CONV2_H 8
#define MNIST_CNN_CONV2_W 8
#define MNIST_CNN_POOL2_H 4
#define MNIST_CNN_POOL2_W 4
#define MNIST_CNN_FLAT_N (MNIST_CNN_CONV2_OUT_C * MNIST_CNN_POOL2_H * MNIST_CNN_POOL2_W)

#if defined(__clang__) || defined(__GNUC__)
#define MNIST_CNN_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define MNIST_CNN_ALWAYS_INLINE inline
#endif

/*
 * Goal: saturate a signed 32-bit value into [0, 255] without using min/max
 * intrinsics.
 * Role in the pipeline: this form avoids generating llvm.smin/llvm.smax style
 * calls that the tutorial llvm2cf path does not lower yet.
 */
static MNIST_CNN_ALWAYS_INLINE uint8_t mnist_cnn_saturate_u8(int32_t value) {
  if (value & ~255) {
    value = ((~value) >> 31) & 255;
  }
  return (uint8_t)value;
}

/*
 * Goal: compute integer max(a, b) with arithmetic/bitwise operations only.
 * Role in the pipeline: this avoids unsigned max intrinsics in max-pooling
 * loops while preserving deterministic behavior.
 */
static MNIST_CNN_ALWAYS_INLINE int32_t mnist_cnn_max_i32(int32_t a, int32_t b) {
  const int32_t diff = b - a;
  return a + (diff & ~(diff >> 31));
}

/*
 * Goal: convert a float pixel intensity into the model's uint8 input tensor.
 * Role in the pipeline: this function implements ONNX QuantizeLinear behavior
 * with nearest rounding and saturation, which is required before the first
 * QLinearConv stage.
 */
static MNIST_CNN_ALWAYS_INLINE uint8_t mnist_cnn_quantize_input(float x) {
  const float inv_scale = 1.0f / MNIST_CNN_INPUT_SCALE;
  int q = (int)(x * inv_scale + (float)MNIST_CNN_INPUT_ZERO_POINT +
                (x >= 0.0f ? 0.5f : -0.5f));
  return mnist_cnn_saturate_u8(q);
}

/*
 * Goal: dequantize a uint8 activation value using a scalar scale/zero-point.
 * Role in the pipeline: this is used after the second convolution and for the
 * final logits output to match the ONNX DequantizeLinear semantics.
 */
static MNIST_CNN_ALWAYS_INLINE float mnist_cnn_dequantize_u8(uint8_t q,
                                                             float scale,
                                                             uint8_t zero_point) {
  return ((float)((int)q - (int)zero_point)) * scale;
}

/*
 * Goal: run the first quantized convolution + bias and emit uint8 activations.
 * Role in the pipeline: this corresponds to the first QLinearConv node in the
 * ONNX graph, including per-output-channel weight scales and int32 bias terms.
 */
static MNIST_CNN_ALWAYS_INLINE void mnist_cnn_conv1_q(
    const uint8_t input_q[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W],
    uint8_t out_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_CONV1_H][MNIST_CNN_CONV1_W]) {
  for (int oc = 0; oc < MNIST_CNN_CONV1_OUT_C; ++oc) {
    const float requant =
        (MNIST_CNN_INPUT_SCALE * mnist_cnn_conv1_w_scale[oc]) /
        MNIST_CNN_CONV1_OUT_SCALE;
    for (int oh = 0; oh < MNIST_CNN_CONV1_H; ++oh) {
      for (int ow = 0; ow < MNIST_CNN_CONV1_W; ++ow) {
        int32_t acc = mnist_cnn_conv1_bias_q[oc];
        for (int kh = 0; kh < MNIST_CNN_KERNEL_K; ++kh) {
          for (int kw = 0; kw < MNIST_CNN_KERNEL_K; ++kw) {
            const int ih = oh + kh;
            const int iw = ow + kw;
            const int32_t x = (int32_t)input_q[ih][iw] - MNIST_CNN_INPUT_ZERO_POINT;
            const int idx = (((oc * 1 + 0) * MNIST_CNN_KERNEL_K + kh) *
                             MNIST_CNN_KERNEL_K) +
                            kw;
            const int32_t w =
                (int32_t)mnist_cnn_conv1_w_q[idx] - mnist_cnn_conv1_w_zero_point[oc];
            acc += x * w;
          }
        }
        const float rq = ((float)acc) * requant;
        const int y = (int)(rq + (float)MNIST_CNN_CONV1_OUT_ZERO_POINT +
                            (rq >= 0.0f ? 0.5f : -0.5f));
        out_q[oc][oh][ow] = mnist_cnn_saturate_u8(y);
      }
    }
  }
}

/*
 * Goal: perform 2x2 max-pooling over uint8 activations.
 * Role in the pipeline: this models the quantized MaxPool between the first and
 * second convolution stages without changing the quantization parameters.
 */
static MNIST_CNN_ALWAYS_INLINE void mnist_cnn_maxpool1_u8(
    const uint8_t in_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_CONV1_H][MNIST_CNN_CONV1_W],
    uint8_t out_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_POOL1_H][MNIST_CNN_POOL1_W]) {
  for (int c = 0; c < MNIST_CNN_CONV1_OUT_C; ++c) {
    for (int oh = 0; oh < MNIST_CNN_POOL1_H; ++oh) {
      for (int ow = 0; ow < MNIST_CNN_POOL1_W; ++ow) {
        const int ih0 = oh * MNIST_CNN_POOL_K;
        const int iw0 = ow * MNIST_CNN_POOL_K;
        int32_t m = (int32_t)in_q[c][ih0][iw0];
        m = mnist_cnn_max_i32(m, (int32_t)in_q[c][ih0][iw0 + 1]);
        m = mnist_cnn_max_i32(m, (int32_t)in_q[c][ih0 + 1][iw0]);
        m = mnist_cnn_max_i32(m, (int32_t)in_q[c][ih0 + 1][iw0 + 1]);
        out_q[c][oh][ow] = (uint8_t)m;
      }
    }
  }
}

/*
 * Goal: run the second quantized convolution + bias and emit uint8 activations.
 * Role in the pipeline: this corresponds to the second QLinearConv node and
 * consumes the pooled uint8 activations from the first block.
 */
static MNIST_CNN_ALWAYS_INLINE void mnist_cnn_conv2_q(
    const uint8_t input_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_POOL1_H][MNIST_CNN_POOL1_W],
    uint8_t out_q[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_CONV2_H][MNIST_CNN_CONV2_W]) {
  for (int oc = 0; oc < MNIST_CNN_CONV2_OUT_C; ++oc) {
    const float requant =
        (MNIST_CNN_POOL1_SCALE * mnist_cnn_conv2_w_scale[oc]) /
        MNIST_CNN_CONV2_OUT_SCALE;
    for (int oh = 0; oh < MNIST_CNN_CONV2_H; ++oh) {
      for (int ow = 0; ow < MNIST_CNN_CONV2_W; ++ow) {
        int32_t acc = mnist_cnn_conv2_bias_q[oc];
        for (int ic = 0; ic < MNIST_CNN_CONV1_OUT_C; ++ic) {
          for (int kh = 0; kh < MNIST_CNN_KERNEL_K; ++kh) {
            for (int kw = 0; kw < MNIST_CNN_KERNEL_K; ++kw) {
              const int ih = oh + kh;
              const int iw = ow + kw;
              const int32_t x = (int32_t)input_q[ic][ih][iw] - MNIST_CNN_POOL1_ZERO_POINT;
              const int idx = ((((oc * MNIST_CNN_CONV1_OUT_C + ic) *
                                 MNIST_CNN_KERNEL_K) +
                                kh) *
                               MNIST_CNN_KERNEL_K) +
                              kw;
              const int32_t w =
                  (int32_t)mnist_cnn_conv2_w_q[idx] - mnist_cnn_conv2_w_zero_point[oc];
              acc += x * w;
            }
          }
        }
        const float rq = ((float)acc) * requant;
        const int y = (int)(rq + (float)MNIST_CNN_CONV2_OUT_ZERO_POINT +
                            (rq >= 0.0f ? 0.5f : -0.5f));
        out_q[oc][oh][ow] = mnist_cnn_saturate_u8(y);
      }
    }
  }
}

/*
 * Goal: dequantize second-convolution activations and apply 2x2 float max-pool.
 * Role in the pipeline: this models the ONNX path where conv2 is dequantized
 * before Pooling160 and produces the 4x4x16 tensor used by the FC layer.
 */
static MNIST_CNN_ALWAYS_INLINE void mnist_cnn_pool2_float(
    const uint8_t in_q[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_CONV2_H][MNIST_CNN_CONV2_W],
    float out_f[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_POOL2_H][MNIST_CNN_POOL2_W]) {
  for (int c = 0; c < MNIST_CNN_CONV2_OUT_C; ++c) {
    for (int oh = 0; oh < MNIST_CNN_POOL2_H; ++oh) {
      for (int ow = 0; ow < MNIST_CNN_POOL2_W; ++ow) {
        const int ih0 = oh * MNIST_CNN_POOL_K;
        const int iw0 = ow * MNIST_CNN_POOL_K;
        float m = mnist_cnn_dequantize_u8(in_q[c][ih0][iw0], MNIST_CNN_CONV2_OUT_SCALE,
                                          MNIST_CNN_CONV2_OUT_ZERO_POINT);
        const float v01 = mnist_cnn_dequantize_u8(in_q[c][ih0][iw0 + 1],
                                                  MNIST_CNN_CONV2_OUT_SCALE,
                                                  MNIST_CNN_CONV2_OUT_ZERO_POINT);
        const float v10 = mnist_cnn_dequantize_u8(in_q[c][ih0 + 1][iw0],
                                                  MNIST_CNN_CONV2_OUT_SCALE,
                                                  MNIST_CNN_CONV2_OUT_ZERO_POINT);
        const float v11 = mnist_cnn_dequantize_u8(in_q[c][ih0 + 1][iw0 + 1],
                                                  MNIST_CNN_CONV2_OUT_SCALE,
                                                  MNIST_CNN_CONV2_OUT_ZERO_POINT);
        if (v01 > m)
          m = v01;
        if (v10 > m)
          m = v10;
        if (v11 > m)
          m = v11;
        out_f[c][oh][ow] = m;
      }
    }
  }
}

/*
 * Goal: flatten pooled features, run quantized FC matmul + quantized bias add,
 * and finally dequantize logits.
 * Role in the pipeline: this implements QLinearMatMul + QLinearAdd + final
 * DequantizeLinear and emits the 10-class floating logits.
 */
static MNIST_CNN_ALWAYS_INLINE void mnist_cnn_fc_logits(
    const float pooled_f[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_POOL2_H][MNIST_CNN_POOL2_W],
    float logits[MNIST_CNN_NUM_CLASSES]) {
  uint8_t flat_q[MNIST_CNN_FLAT_N];
  int p = 0;
  for (int c = 0; c < MNIST_CNN_CONV2_OUT_C; ++c) {
    for (int h = 0; h < MNIST_CNN_POOL2_H; ++h) {
      for (int w = 0; w < MNIST_CNN_POOL2_W; ++w) {
        const float x = pooled_f[c][h][w];
        const int q = (int)(x / MNIST_CNN_FLAT_SCALE + (float)MNIST_CNN_FLAT_ZERO_POINT +
                            (x >= 0.0f ? 0.5f : -0.5f));
        flat_q[p++] = mnist_cnn_saturate_u8(q);
      }
    }
  }

  uint8_t matmul_q[MNIST_CNN_NUM_CLASSES];
  for (int oc = 0; oc < MNIST_CNN_NUM_CLASSES; ++oc) {
    int32_t acc = 0;
    for (int i = 0; i < MNIST_CNN_FLAT_N; ++i) {
      const int32_t a = (int32_t)flat_q[i] - MNIST_CNN_FLAT_ZERO_POINT;
      const int32_t b = (int32_t)mnist_cnn_fc_w_q[i * MNIST_CNN_NUM_CLASSES + oc] -
                        mnist_cnn_fc_w_zero_point[oc];
      acc += a * b;
    }
    const float rq = ((float)acc) *
                     (MNIST_CNN_FLAT_SCALE * mnist_cnn_fc_w_scale[oc] /
                      MNIST_CNN_FC_MM_SCALE);
    const int y = (int)(rq + (float)MNIST_CNN_FC_MM_ZERO_POINT +
                        (rq >= 0.0f ? 0.5f : -0.5f));
    matmul_q[oc] = mnist_cnn_saturate_u8(y);
  }

  for (int oc = 0; oc < MNIST_CNN_NUM_CLASSES; ++oc) {
    const float a =
        ((float)((int)matmul_q[oc] - MNIST_CNN_FC_MM_ZERO_POINT)) * MNIST_CNN_FC_MM_SCALE;
    const float b =
        ((float)((int)mnist_cnn_fc_bias_q[oc] - MNIST_CNN_FC_BIAS_ZERO_POINT)) *
        MNIST_CNN_FC_BIAS_SCALE;
    const float sum = a + b;
    const int y = (int)(sum / MNIST_CNN_LOGITS_SCALE + (float)MNIST_CNN_LOGITS_ZERO_POINT +
                        (sum >= 0.0f ? 0.5f : -0.5f));
    const uint8_t q = mnist_cnn_saturate_u8(y);
    logits[oc] = mnist_cnn_dequantize_u8(q, MNIST_CNN_LOGITS_SCALE,
                                         MNIST_CNN_LOGITS_ZERO_POINT);
  }
}

/*
 * Inline implementation shared by both the kernel entry point and reference
 * harness.  This macro expands to the previously inlined nested loops and
 * keeps the data layout identical across both consumers without introducing
 * an extra call boundary.
 */
#define MNIST_CNN_INFER_BODY(IN_INPUT, OUT_LOGITS)                                      \
  do {                                                                                 \
    uint8_t input_q[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W];                             \
    uint8_t conv1_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_CONV1_H][MNIST_CNN_CONV1_W];      \
    uint8_t pool1_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_POOL1_H][MNIST_CNN_POOL1_W];      \
    uint8_t conv2_q[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_CONV2_H][MNIST_CNN_CONV2_W];      \
    float pool2_f[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_POOL2_H][MNIST_CNN_POOL2_W];        \
                                                                                       \
    for (int h = 0; h < MNIST_CNN_INPUT_H; ++h) {                                      \
      for (int w = 0; w < MNIST_CNN_INPUT_W; ++w) {                                    \
        input_q[h][w] = mnist_cnn_quantize_input((IN_INPUT)[h][w]);                    \
      }                                                                                \
    }                                                                                  \
                                                                                       \
    mnist_cnn_conv1_q(input_q, conv1_q);                                               \
    mnist_cnn_maxpool1_u8(conv1_q, pool1_q);                                           \
    mnist_cnn_conv2_q(pool1_q, conv2_q);                                               \
    mnist_cnn_pool2_float(conv2_q, pool2_f);                                           \
    mnist_cnn_fc_logits(pool2_f, (OUT_LOGITS));                                        \
  } while (0)

/*
 * Goal: execute the full LeNet-style inference path used by this benchmark.
 * Role in the pipeline: this function composes all quantized/dequantized stages
 * into one reusable implementation for both the HLS top function and the
 * software reference, ensuring deterministic behavior across all flows.
 */
static MNIST_CNN_ALWAYS_INLINE void mnist_cnn_infer_impl(
    const float input[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W],
    float logits[MNIST_CNN_NUM_CLASSES]) {
  MNIST_CNN_INFER_BODY(input, logits);
}

#endif
