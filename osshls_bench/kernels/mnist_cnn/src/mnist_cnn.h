#ifndef MNIST_CNN_H
#define MNIST_CNN_H

#define MNIST_CNN_INPUT_H 28
#define MNIST_CNN_INPUT_W 28
#define MNIST_CNN_CONV1_OUT_C 4
#define MNIST_CNN_CONV2_OUT_C 8
#define MNIST_CNN_KERNEL_K 5
#define MNIST_CNN_POOL_K 2
#define MNIST_CNN_NUM_CLASSES 10

#ifdef __cplusplus
extern "C" {
#endif

void mnist_cnn(const float input[MNIST_CNN_INPUT_H][MNIST_CNN_INPUT_W],
               float logits[MNIST_CNN_NUM_CLASSES]);

#ifdef __cplusplus
}
#endif

#endif
