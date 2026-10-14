#ifndef PASSTHROUGH_H
#define PASSTHROUGH_H

#define PASSTHROUGH_N 1024

/*
 * Top-level passthrough kernel declaration.
 *
 * Role:
 * Copy PASSTHROUGH_N integer samples from data_in to data_out.
 *
 * Goal:
 * Provide one stable prototype shared by the kernel implementation and software
 * testbench so all HLS runners synthesize the same memory-to-memory copy entry
 * point.
 */
void passthrough(const int data_in[PASSTHROUGH_N], int data_out[PASSTHROUGH_N]);

#endif
