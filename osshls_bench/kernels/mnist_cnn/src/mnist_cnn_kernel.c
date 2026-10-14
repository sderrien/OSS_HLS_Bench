#include "mnist_cnn.h"
#include "mnist_cnn_model.h"
#include <stdio.h>
#include <ohls_trace.h>

/*
 * Goal: keep kernel-stage checksum traces available for local debugging while
 * preserving default compatibility with compiled CycleSim/RTL flows.
 * Role in benchmark stability: by default these traces are disabled so the
 * lowered kernel does not emit specialized OHLS printf helper symbols.
 */
#ifndef MNIST_CNN_KERNEL_STAGE_TRACE
#define MNIST_CNN_KERNEL_STAGE_TRACE 0
#endif

#if MNIST_CNN_KERNEL_STAGE_TRACE
#define MNIST_CNN_TRACE_U64(LABEL, VALUE)                                           \
  printf("[mnist-kernel-stage] " LABEL "=%llu\\n", (unsigned long long)(VALUE))
#define MNIST_CNN_TRACE_I32(LABEL, VALUE)                                           \
  printf("[mnist-kernel-stage] " LABEL "=%d\\n", (int)(VALUE))
#else
#define MNIST_CNN_TRACE_U64(LABEL, VALUE) ((void)0)
#define MNIST_CNN_TRACE_I32(LABEL, VALUE) ((void)0)
#endif

#define MNIST_CNN_CLAMP_UINT8(VALUE)                                              \
  ((((int32_t)(VALUE)) & ~255)                                                       \
       ? (((~(int32_t)(VALUE)) >> 31) & 255)                                         \
       : ((int32_t)(VALUE)))

#define MNIST_CNN_QUANTIZE_INPUT(X)                                               \
  MNIST_CNN_CLAMP_UINT8((int)((X) * (1.0f / MNIST_CNN_INPUT_SCALE) +              \
                             (float)MNIST_CNN_INPUT_ZERO_POINT +                    \
                             ((X) >= 0.0f ? 0.5f : -0.5f)))


#pragma HLS top
/*
 * Goal: expose the quantized LeNet-style inference as a single synthesizable
 * entry point so the kernel DAG does not contain any unknown helper calls.
 * Role in the benchmark: every stage of the pipeline is written inline inside
 * `mnist_cnn`, so the generated LLVM IR is guaranteed to expose one opaque
 * top function and stays acceptable for the hyperblock pass.
 */
void mnist_cnn(const float input[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W],
               float logits[MNIST_CNN_NUM_CLASSES]) {
  uint8_t input_q[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W];
  uint8_t conv1_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_CONV1_H][MNIST_CNN_CONV1_W];
  uint8_t pool1_q[MNIST_CNN_CONV1_OUT_C][MNIST_CNN_POOL1_H][MNIST_CNN_POOL1_W];
  uint8_t conv2_q[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_CONV2_H][MNIST_CNN_CONV2_W];
  float pool2_f[MNIST_CNN_CONV2_OUT_C][MNIST_CNN_POOL2_H][MNIST_CNN_POOL2_W];

mnist_quant_input_h:
  for (int h = 0; h < MNIST_CNN_INPUT_H; ++h) {
mnist_quant_input_w:
    #pragma HLS pipeline
    for (int w = 0; w < MNIST_CNN_INPUT_W; ++w) {
      input_q[h][w] = MNIST_CNN_QUANTIZE_INPUT(input[h][w]);
    }
  }
  {
    uint64_t c = 0;
mnist_input_checksum_h:
    for (int h = 0; h < MNIST_CNN_INPUT_H; ++h)
mnist_input_checksum_w:
      #pragma HLS pipeline
    for (int w = 0; w < MNIST_CNN_INPUT_W; ++w)
        c = (c * 1099511628211ULL) ^ (uint64_t)input_q[h][w];
    MNIST_CNN_TRACE_U64("input_q_checksum", c);
  }

mnist_conv1_oc:
  for (int oc = 0; oc < MNIST_CNN_CONV1_OUT_C; ++oc) {
    const float requant =
        (MNIST_CNN_INPUT_SCALE * mnist_cnn_conv1_w_scale[oc]) /
        MNIST_CNN_CONV1_OUT_SCALE;
mnist_conv1_oh:
    for (int oh = 0; oh < MNIST_CNN_CONV1_H; ++oh) {
mnist_conv1_ow:
      for (int ow = 0; ow < MNIST_CNN_CONV1_W; ++ow) {
        int32_t acc = mnist_cnn_conv1_bias_q[oc];
mnist_conv1_kh:
        for (int kh = 0; kh < MNIST_CNN_KERNEL_K; ++kh) {
mnist_conv1_kw:
          #pragma HLS pipeline
          for (int kw = 0; kw < MNIST_CNN_KERNEL_K; ++kw) {
            const int ih = oh + kh;
            const int iw = ow + kw;
            const int32_t x = (int32_t)input_q[ih][iw] - MNIST_CNN_INPUT_ZERO_POINT;
            const int idx = (((oc * MNIST_CNN_KERNEL_K + kh) *
                              MNIST_CNN_KERNEL_K) +
                             kw);
            const int32_t w =
                (int32_t)mnist_cnn_conv1_w_q[idx] -
                mnist_cnn_conv1_w_zero_point[oc];
            acc += x * w;
          }
        }
        const float rq = ((float)acc) * requant;
        const int y = (int)(rq + (float)MNIST_CNN_CONV1_OUT_ZERO_POINT +
                            (rq >= 0.0f ? 0.5f : -0.5f));
        conv1_q[oc][oh][ow] = (uint8_t)MNIST_CNN_CLAMP_UINT8(y);
      }
    }
  }
  {
    uint64_t c = 0;
mnist_conv1_checksum_oc:
    for (int oc = 0; oc < MNIST_CNN_CONV1_OUT_C; ++oc)
mnist_conv1_checksum_h:
      for (int h = 0; h < MNIST_CNN_CONV1_H; ++h)
mnist_conv1_checksum_w:
        #pragma HLS pipeline
        for (int w = 0; w < MNIST_CNN_CONV1_W; ++w)
          c = (c * 1099511628211ULL) ^ (uint64_t)conv1_q[oc][h][w];
    MNIST_CNN_TRACE_U64("conv1_q_checksum", c);
  }

mnist_pool1_c:
  for (int c = 0; c < MNIST_CNN_CONV1_OUT_C; ++c) {
mnist_pool1_oh:
    for (int oh = 0; oh < MNIST_CNN_POOL1_H; ++oh) {
mnist_pool1_ow:
      #pragma HLS pipeline
      for (int ow = 0; ow < MNIST_CNN_POOL1_W; ++ow) {
        const int ih0 = oh * MNIST_CNN_POOL_K;
        const int iw0 = ow * MNIST_CNN_POOL_K;
        int32_t m = (int32_t)conv1_q[c][ih0][iw0];
        int32_t candidate = (int32_t)conv1_q[c][ih0][iw0 + 1];
        if (candidate > m)
          m = candidate;
        candidate = (int32_t)conv1_q[c][ih0 + 1][iw0];
        if (candidate > m)
          m = candidate;
        candidate = (int32_t)conv1_q[c][ih0 + 1][iw0 + 1];
        if (candidate > m)
          m = candidate;
        pool1_q[c][oh][ow] = (uint8_t)m;
      }
    }
  }
  {
    uint64_t c = 0;
mnist_pool1_checksum_oc:
    for (int oc = 0; oc < MNIST_CNN_CONV1_OUT_C; ++oc)
mnist_pool1_checksum_h:
      for (int h = 0; h < MNIST_CNN_POOL1_H; ++h)
mnist_pool1_checksum_w:
        #pragma HLS pipeline
        for (int w = 0; w < MNIST_CNN_POOL1_W; ++w)
          c = (c * 1099511628211ULL) ^ (uint64_t)pool1_q[oc][h][w];
    MNIST_CNN_TRACE_U64("pool1_q_checksum", c);
  }

mnist_conv2_oc:
  for (int oc = 0; oc < MNIST_CNN_CONV2_OUT_C; ++oc) {
    const float requant =
        (MNIST_CNN_POOL1_SCALE * mnist_cnn_conv2_w_scale[oc]) /
        MNIST_CNN_CONV2_OUT_SCALE;
mnist_conv2_oh:
    for (int oh = 0; oh < MNIST_CNN_CONV2_H; ++oh) {
mnist_conv2_ow:
      for (int ow = 0; ow < MNIST_CNN_CONV2_W; ++ow) {
        int32_t acc = mnist_cnn_conv2_bias_q[oc];
mnist_conv2_ic:
        for (int ic = 0; ic < MNIST_CNN_CONV1_OUT_C; ++ic) {
mnist_conv2_kh:
          for (int kh = 0; kh < MNIST_CNN_KERNEL_K; ++kh) {
mnist_conv2_kw:
            #pragma HLS pipeline
            for (int kw = 0; kw < MNIST_CNN_KERNEL_K; ++kw) {
              const int ih = oh + kh;
              const int iw = ow + kw;
              const int32_t x =
                  (int32_t)pool1_q[ic][ih][iw] - MNIST_CNN_POOL1_ZERO_POINT;
              const int idx = ((((oc * MNIST_CNN_CONV1_OUT_C + ic) *
                                 MNIST_CNN_KERNEL_K) +
                                kh) * MNIST_CNN_KERNEL_K) +
                              kw;
              const int32_t w =
                  (int32_t)mnist_cnn_conv2_w_q[idx] -
                  mnist_cnn_conv2_w_zero_point[oc];
              acc += x * w;
            }
          }
        }
        const float rq = ((float)acc) * requant;
        const int y = (int)(rq + (float)MNIST_CNN_CONV2_OUT_ZERO_POINT +
                            (rq >= 0.0f ? 0.5f : -0.5f));
        conv2_q[oc][oh][ow] = (uint8_t)MNIST_CNN_CLAMP_UINT8(y);
      }
    }
  }
  {
    uint64_t c = 0;
mnist_conv2_checksum_oc:
    for (int oc = 0; oc < MNIST_CNN_CONV2_OUT_C; ++oc)
mnist_conv2_checksum_h:
      for (int h = 0; h < MNIST_CNN_CONV2_H; ++h)
mnist_conv2_checksum_w:
        #pragma HLS pipeline
        for (int w = 0; w < MNIST_CNN_CONV2_W; ++w)
          c = (c * 1099511628211ULL) ^ (uint64_t)conv2_q[oc][h][w];
    MNIST_CNN_TRACE_U64("conv2_q_checksum", c);
  }


mnist_pool2_c:
  for (int c = 0; c < MNIST_CNN_CONV2_OUT_C; ++c) {
mnist_pool2_oh:
    for (int oh = 0; oh < MNIST_CNN_POOL2_H; ++oh) {
mnist_pool2_ow:
      #pragma HLS pipeline
      for (int ow = 0; ow < MNIST_CNN_POOL2_W; ++ow) {
        const int ih0 = oh * MNIST_CNN_POOL_K;
        const int iw0 = ow * MNIST_CNN_POOL_K;
        float m =
            ((float)((int)conv2_q[c][ih0][iw0] - MNIST_CNN_CONV2_OUT_ZERO_POINT)) *
            MNIST_CNN_CONV2_OUT_SCALE;
        float candidate =
            ((float)((int)conv2_q[c][ih0][iw0 + 1] -
                     MNIST_CNN_CONV2_OUT_ZERO_POINT)) *
            MNIST_CNN_CONV2_OUT_SCALE;
        if (candidate > m)
          m = candidate;
        candidate =
            ((float)((int)conv2_q[c][ih0 + 1][iw0] -
                     MNIST_CNN_CONV2_OUT_ZERO_POINT)) *
            MNIST_CNN_CONV2_OUT_SCALE;
        if (candidate > m)
          m = candidate;
        candidate =
            ((float)((int)conv2_q[c][ih0 + 1][iw0 + 1] -
                     MNIST_CNN_CONV2_OUT_ZERO_POINT)) *
            MNIST_CNN_CONV2_OUT_SCALE;
        if (candidate > m)
          m = candidate;
        pool2_f[c][oh][ow] = m;
      }
    }
  }
  {
    int32_t c = 0;
mnist_pool2_checksum_oc:
    for (int oc = 0; oc < MNIST_CNN_CONV2_OUT_C; ++oc) {
mnist_pool2_checksum_h:
      for (int h = 0; h < MNIST_CNN_POOL2_H; ++h) {
mnist_pool2_checksum_w:
        #pragma HLS pipeline
        for (int w = 0; w < MNIST_CNN_POOL2_W; ++w) {
          const int32_t scaled = (int32_t)(pool2_f[oc][h][w] * 1024.0f);
          c = c * 1315423911 + scaled;
        }
      }
    }
    MNIST_CNN_TRACE_I32("pool2_f_checksum", c);
  }

  uint8_t flat_q[MNIST_CNN_FLAT_N];
  int flat_index = 0;
  const float inv_flat_scale = 1.0f / MNIST_CNN_FLAT_SCALE;
mnist_flatten_c:
  for (int c = 0; c < MNIST_CNN_CONV2_OUT_C; ++c) {
mnist_flatten_h:
    for (int h = 0; h < MNIST_CNN_POOL2_H; ++h) {
mnist_flatten_w:
      #pragma HLS pipeline
      for (int w = 0; w < MNIST_CNN_POOL2_W; ++w) {
        const float x = pool2_f[c][h][w];
        const int q = (int)(x * inv_flat_scale +
                            (float)MNIST_CNN_FLAT_ZERO_POINT +
                            (x >= 0.0f ? 0.5f : -0.5f));
        uint8_t flat_q_value = (uint8_t)MNIST_CNN_CLAMP_UINT8(q);
        flat_q[flat_index++] = flat_q_value;
      }
    }
  }
  
  uint8_t matmul_q[MNIST_CNN_NUM_CLASSES];
mnist_fc_oc:
  for (int oc = 0; oc < MNIST_CNN_NUM_CLASSES; ++oc) {
    int32_t acc = 0;
mnist_fc_i:
    #pragma HLS pipeline
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
    uint8_t matmul_q_value = (uint8_t)MNIST_CNN_CLAMP_UINT8(y);
    matmul_q[oc] = matmul_q_value;
    __ohls_vcd_trace_i8("matmul_q", matmul_q[oc]);
  }

mnist_logits_oc:
  for (int oc = 0; oc < MNIST_CNN_NUM_CLASSES; ++oc) {
    const float a =
        ((float)((int)matmul_q[oc] - MNIST_CNN_FC_MM_ZERO_POINT)) *
        MNIST_CNN_FC_MM_SCALE;
    const float b =
        ((float)((int)mnist_cnn_fc_bias_q[oc] - MNIST_CNN_FC_BIAS_ZERO_POINT)) *
        MNIST_CNN_FC_BIAS_SCALE;
    const float sum = a + b;
    const int y = (int)(sum / MNIST_CNN_LOGITS_SCALE +
                        (float)MNIST_CNN_LOGITS_ZERO_POINT +
                        (sum >= 0.0f ? 0.5f : -0.5f));
    const uint8_t q = (uint8_t)MNIST_CNN_CLAMP_UINT8(y);
    logits[oc] = ((float)((int)q - MNIST_CNN_LOGITS_ZERO_POINT)) *
                 MNIST_CNN_LOGITS_SCALE;
  }
  {
    int32_t c = 0;
mnist_logits_checksum_i:
    #pragma HLS pipeline
    for (int i = 0; i < MNIST_CNN_NUM_CLASSES; ++i) {
      const int32_t scaled = (int32_t)(logits[i] * 1024.0f);
      c = c * 1315423911 + scaled;
    }
    MNIST_CNN_TRACE_I32("logits_checksum", c);
  }
}
