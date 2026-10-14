#ifndef QR_GIVENS_H
#define QR_GIVENS_H

#define N 8

/*
 * Role:
 * Declare the tutorial top-level Givens-rotation kernel interface.
 *
 * Goal:
 * Keep the native software signature for regular host verification while
 * exposing the Verilator wrapper ABI (i32 pointer memories) during
 * rtl-runtime builds.
 */
#ifdef __cplusplus
extern "C" {
#endif

void qr_givens(const float a[N][N], float r[N][N]);

#ifdef __cplusplus
}
#endif

#endif /* QR_GIVENS_H */
