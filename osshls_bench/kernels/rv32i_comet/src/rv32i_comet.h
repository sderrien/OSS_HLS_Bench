#ifndef RV32I_COMET_H
#define RV32I_COMET_H

#include <stdint.h>

#define RV32I_COMET_IMEM_WORDS 64
#define RV32I_COMET_DMEM_WORDS 64
#define RV32I_COMET_MAX_CYCLES 64

#define RV32I_OPCODE_LOAD 0x03u
#define RV32I_OPCODE_STORE 0x23u
#define RV32I_OPCODE_BRANCH 0x63u
#define RV32I_OPCODE_OP_IMM 0x13u
#define RV32I_OPCODE_OP 0x33u

#define RV32I_FUNCT3_ADDI 0x0u
#define RV32I_FUNCT3_ADD_SUB 0x0u
#define RV32I_FUNCT3_LW 0x2u
#define RV32I_FUNCT3_SW 0x2u
#define RV32I_FUNCT3_BNE 0x1u
#define RV32I_FUNCT7_SUB 0x20u

#ifdef __cplusplus
extern "C" {
#endif

int rv32i_comet_run(uint32_t instr_mem[RV32I_COMET_IMEM_WORDS],
                    uint32_t data_mem[RV32I_COMET_DMEM_WORDS]);

#ifdef __cplusplus
}
#endif

#endif
