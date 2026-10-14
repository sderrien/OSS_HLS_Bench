#include <stdint.h>
#include <stdio.h>

#include "rv32i_comet.h"

void __hls_pipeline_marker(int, int, int) {}

static uint32_t enc_addi(uint8_t rd, uint8_t rs1, int imm) {
  return (((uint32_t)imm & 0xfffu) << 20) | ((uint32_t)rs1 << 15) |
         (RV32I_FUNCT3_ADDI << 12) | ((uint32_t)rd << 7) | RV32I_OPCODE_OP_IMM;
}

static uint32_t enc_sub(uint8_t rd, uint8_t rs1, uint8_t rs2) {
  return (RV32I_FUNCT7_SUB << 25) | ((uint32_t)rs2 << 20) |
         ((uint32_t)rs1 << 15) | (RV32I_FUNCT3_ADD_SUB << 12) |
         ((uint32_t)rd << 7) | RV32I_OPCODE_OP;
}

static uint32_t enc_lw(uint8_t rd, uint8_t rs1, int imm) {
  return (((uint32_t)imm & 0xfffu) << 20) | ((uint32_t)rs1 << 15) |
         (RV32I_FUNCT3_LW << 12) | ((uint32_t)rd << 7) | RV32I_OPCODE_LOAD;
}

static uint32_t enc_sw(uint8_t rs2, uint8_t rs1, int imm) {
  uint32_t uimm = (uint32_t)imm & 0xfffu;
  return (((uimm >> 5) & 0x7fu) << 25) | ((uint32_t)rs2 << 20) |
         ((uint32_t)rs1 << 15) | (RV32I_FUNCT3_SW << 12) |
         ((uimm & 0x1fu) << 7) | RV32I_OPCODE_STORE;
}

static uint32_t enc_bne(uint8_t rs1, uint8_t rs2, int imm) {
  uint32_t uimm = (uint32_t)imm & 0x1fffu;
  return (((uimm >> 12) & 0x1u) << 31) | (((uimm >> 5) & 0x3fu) << 25) |
         ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) |
         (RV32I_FUNCT3_BNE << 12) | (((uimm >> 1) & 0x0fu) << 8) |
         (((uimm >> 11) & 0x1u) << 7) | RV32I_OPCODE_BRANCH;
}

int main(int argc, char **argv) {
  uint32_t instr_mem[RV32I_COMET_IMEM_WORDS] = {0};
  uint32_t data_mem[RV32I_COMET_DMEM_WORDS] = {0};

  instr_mem[0] = enc_addi(1, 0, 5);    // x1 = loop count
  instr_mem[1] = enc_addi(2, 0, 0);    // x2 = sum
  instr_mem[2] = enc_addi(2, 2, 3);    // x2 += 3
  instr_mem[3] = enc_addi(1, 1, -1);   // x1--
  instr_mem[4] = enc_bne(1, 0, -8);    // loop while x1 != 0
  instr_mem[5] = enc_sw(2, 0, 0);      // mem[0] = 15
  instr_mem[6] = enc_lw(3, 0, 0);      // x3 = mem[0]
  instr_mem[7] = enc_addi(4, 0, 15);   // x4 = expected
  instr_mem[8] = enc_sub(5, 3, 4);     // x5 = loaded - expected
  instr_mem[9] = enc_sw(5, 0, 4);      // mem[1] = 0 on pass
  instr_mem[10] = 0;                   // stop fetching and drain

  int ret = rv32i_comet_run(instr_mem, data_mem);
  int ok = data_mem[0] == 15 && data_mem[1] == 0 && ret == 15;

  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      perror("fopen");
      return 1;
    }
  }
  fprintf(out, "result=%u\n", data_mem[0]);
  fprintf(out, "check=%u\n", data_mem[1]);
  fprintf(out, "return=%d\n", ret);
  fprintf(out, "pass=%s\n", ok ? "yes" : "no");
  if (out != stdout) {
    fclose(out);
  }
  return ok ? 0 : 2;
}
