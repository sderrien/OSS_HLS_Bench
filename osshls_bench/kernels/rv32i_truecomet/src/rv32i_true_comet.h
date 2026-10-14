#ifndef RV32I_TRUE_COMET_H
#define RV32I_TRUE_COMET_H

#include <stdint.h>

/*
 * This header defines the small fixed-memory ABI used by the directed OpenHLS
 * benchmark. It intentionally matches rv32i_comet's wrapper-facing interface
 * while the kernel implements Comet's explicit cycle-transition structure.
 */
#define RV32I_TRUE_COMET_IMEM_WORDS 64
#define RV32I_TRUE_COMET_DMEM_WORDS 64
#define RV32I_TRUE_COMET_DMEM_BYTES (RV32I_TRUE_COMET_DMEM_WORDS * 4)
#define RV32I_TRUE_COMET_MAX_CYCLES 64

#define RV32I_TRUE_COMET_STATUS_ECALL 0
#define RV32I_TRUE_COMET_STATUS_EBREAK 1
#define RV32I_TRUE_COMET_STATUS_ILLEGAL_INSTRUCTION 2
#define RV32I_TRUE_COMET_STATUS_MISALIGNED_INSTRUCTION 3
#define RV32I_TRUE_COMET_STATUS_MISALIGNED_DATA 4
#define RV32I_TRUE_COMET_STATUS_TIMEOUT 5
#define RV32I_TRUE_COMET_STATUS_INSTRUCTION_ACCESS 6
#define RV32I_TRUE_COMET_STATUS_LOAD_ACCESS 7
#define RV32I_TRUE_COMET_STATUS_STORE_ACCESS 8

#define RV32I_TRUE_COMET_OPCODE_LOAD 0x03u
#define RV32I_TRUE_COMET_OPCODE_MISC_MEM 0x0fu
#define RV32I_TRUE_COMET_OPCODE_OP_IMM 0x13u
#define RV32I_TRUE_COMET_OPCODE_AUIPC 0x17u
#define RV32I_TRUE_COMET_OPCODE_STORE 0x23u
#define RV32I_TRUE_COMET_OPCODE_BRANCH 0x63u
#define RV32I_TRUE_COMET_OPCODE_OP 0x33u
#define RV32I_TRUE_COMET_OPCODE_LUI 0x37u
#define RV32I_TRUE_COMET_OPCODE_SYSTEM 0x73u
#define RV32I_TRUE_COMET_OPCODE_JALR 0x67u
#define RV32I_TRUE_COMET_OPCODE_JAL 0x6fu

#define RV32I_TRUE_COMET_FUNCT3_ADDI 0x0u
#define RV32I_TRUE_COMET_FUNCT3_ADD_SUB 0x0u
#define RV32I_TRUE_COMET_FUNCT3_LW 0x2u
#define RV32I_TRUE_COMET_FUNCT3_SW 0x2u
#define RV32I_TRUE_COMET_FUNCT3_BNE 0x1u
#define RV32I_TRUE_COMET_FUNCT7_SUB 0x20u

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Run a finite directed execution of the Comet-structured five-stage core.
 * Instruction memory remains word-addressable. Data memory is logically
 * byte-addressed and transported over a 32-bit BRAM interface with byte enables.
 * The return value is an
 * RV32I_TRUE_COMET_STATUS_* terminal outcome rather than an architectural data
 * value.
 */
int rv32i_true_comet_run(
    uint32_t instr_mem[RV32I_TRUE_COMET_IMEM_WORDS],
    uint8_t data_mem[RV32I_TRUE_COMET_DMEM_BYTES]);

#ifdef __cplusplus
}
#endif

#endif
