#include <stdint.h>
#include <stdio.h>

#include "rv32i_true_comet.h"

extern "C" void __hls_pipeline_marker(int, int, int) {}

/* Build one I-format instruction for arithmetic, loads, JALR, and SYSTEM tests. */
static uint32_t enc_i(uint8_t opcode, uint8_t funct3, uint8_t rd, uint8_t rs1, int imm) {
  return (((uint32_t)imm & 0xfffu) << 20) | ((uint32_t)rs1 << 15) |
         ((uint32_t)funct3 << 12) | ((uint32_t)rd << 7) | opcode;
}

/* Build one R-format instruction, including the ADD/SUB and SRL/SRA funct7 bit. */
static uint32_t enc_r(uint8_t funct7, uint8_t funct3, uint8_t rd, uint8_t rs1, uint8_t rs2) {
  return ((uint32_t)funct7 << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) |
         ((uint32_t)funct3 << 12) | ((uint32_t)rd << 7) | RV32I_TRUE_COMET_OPCODE_OP;
}

/* Build one S-format store with its immediate split around rs1 and rs2. */
static uint32_t enc_s(uint8_t funct3, uint8_t rs2, uint8_t rs1, int imm) {
  uint32_t uimm = (uint32_t)imm & 0xfffu;
  return (((uimm >> 5) & 0x7fu) << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) |
         ((uint32_t)funct3 << 12) | ((uimm & 0x1fu) << 7) | RV32I_TRUE_COMET_OPCODE_STORE;
}

/* Build one B-format conditional branch with a signed, two-byte-aligned offset. */
static uint32_t enc_b(uint8_t funct3, uint8_t rs1, uint8_t rs2, int imm) {
  uint32_t uimm = (uint32_t)imm & 0x1fffu;
  return (((uimm >> 12) & 1u) << 31) | (((uimm >> 5) & 0x3fu) << 25) |
         ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) | ((uint32_t)funct3 << 12) |
         (((uimm >> 1) & 0xfu) << 8) | (((uimm >> 11) & 1u) << 7) |
         RV32I_TRUE_COMET_OPCODE_BRANCH;
}

/* Build one U-format LUI or AUIPC instruction from its 20-bit upper immediate. */
static uint32_t enc_u(uint8_t opcode, uint8_t rd, uint32_t imm20) {
  return (imm20 << 12) | ((uint32_t)rd << 7) | opcode;
}

/* Build one J-format JAL instruction with its non-contiguous signed offset. */
static uint32_t enc_j(uint8_t rd, int imm) {
  uint32_t uimm = (uint32_t)imm & 0x1fffffu;
  return (((uimm >> 20) & 1u) << 31) | (((uimm >> 1) & 0x3ffu) << 21) |
         (((uimm >> 11) & 1u) << 20) | (((uimm >> 12) & 0xffu) << 12) |
         ((uint32_t)rd << 7) | RV32I_TRUE_COMET_OPCODE_JAL;
}

/* Reconstruct one little-endian word from the byte-addressed memory ABI. */
static uint32_t dmem_word(const uint8_t data_mem[RV32I_TRUE_COMET_DMEM_BYTES],
                          unsigned index) {
  unsigned base = index * 4u;
  return (uint32_t)data_mem[base] | ((uint32_t)data_mem[base + 1] << 8u) |
         ((uint32_t)data_mem[base + 2] << 16u) |
         ((uint32_t)data_mem[base + 3] << 24u);
}

/* Run a short program containing each arithmetic and little-endian memory width. */
static bool check_arithmetic_and_memory(void) {
  uint32_t imem[RV32I_TRUE_COMET_IMEM_WORDS] = {};
  uint8_t dmem[RV32I_TRUE_COMET_DMEM_BYTES] = {};
  imem[0] = enc_u(RV32I_TRUE_COMET_OPCODE_LUI, 1, 0x11223);
  imem[1] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 1, 1, 0x344);
  imem[2] = enc_s(2, 1, 0, 0);
  imem[3] = enc_i(RV32I_TRUE_COMET_OPCODE_LOAD, 0, 2, 0, 0);
  imem[4] = enc_i(RV32I_TRUE_COMET_OPCODE_LOAD, 4, 3, 0, 3);
  imem[5] = enc_i(RV32I_TRUE_COMET_OPCODE_LOAD, 1, 4, 0, 0);
  imem[6] = enc_i(RV32I_TRUE_COMET_OPCODE_LOAD, 5, 5, 0, 2);
  imem[7] = enc_s(0, 3, 0, 4);
  imem[8] = enc_s(1, 4, 0, 6);
  imem[9] = enc_r(0, 0, 6, 2, 3);
  imem[10] = enc_r(0x20, 0, 7, 3, 2);
  imem[11] = enc_r(0, 1, 8, 3, 2);
  imem[12] = enc_r(0, 2, 9, 2, 3);
  imem[13] = enc_r(0, 3, 10, 3, 2);
  imem[14] = enc_r(0, 4, 11, 2, 3);
  imem[15] = enc_r(0, 5, 12, 1, 2);
  imem[16] = enc_r(0x20, 5, 13, 1, 2);
  imem[17] = enc_r(0, 6, 14, 2, 3);
  imem[18] = enc_r(0, 7, 15, 2, 3);
  imem[19] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 1, 16, 3, 1);
  imem[20] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 5, 17, 1, 4);
  imem[21] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 5, 18, 1, 0x404);
  imem[22] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 2, 19, 2, 0x100);
  imem[23] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 3, 20, 2, 0x100);
  imem[24] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 4, 21, 2, 0x7f);
  imem[25] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 6, 22, 2, 0x80);
  imem[26] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 7, 23, 2, 0x7f);
  imem[27] = RV32I_TRUE_COMET_OPCODE_MISC_MEM;
  imem[28] = enc_s(2, 6, 0, 8);
  imem[29] = enc_s(2, 7, 0, 12);
  imem[30] = enc_s(2, 8, 0, 16);
  imem[31] = enc_s(2, 13, 0, 20);
  imem[32] = enc_s(2, 16, 0, 24);
  imem[33] = enc_s(2, 18, 0, 28);
  imem[34] = 0x00000073u;
  int status = rv32i_true_comet_run(imem, dmem);
  return status == RV32I_TRUE_COMET_STATUS_ECALL &&
         dmem_word(dmem, 0) == 0x11223344u && dmem_word(dmem, 1) == 0x33440011u && dmem_word(dmem, 2) == 0x00000055u &&
         dmem_word(dmem, 3) == 0xffffffcdu && dmem_word(dmem, 4) == 0x00000110u && dmem_word(dmem, 5) == 0x01122334u &&
         dmem_word(dmem, 6) == 0x00000022u && dmem_word(dmem, 7) == 0x01122334u;
}

/* Exercise all six branch predicates plus AUIPC, JAL, and JALR redirect paths. */
static bool check_control_flow(void) {
  uint32_t imem[RV32I_TRUE_COMET_IMEM_WORDS] = {};
  uint8_t dmem[RV32I_TRUE_COMET_DMEM_BYTES] = {};
  imem[0] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 1, 0, 1);
  imem[1] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 2, 0, 2);
  imem[2] = enc_b(0, 1, 1, 8); imem[3] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 31, 0, 99);
  imem[4] = enc_b(1, 1, 2, 8); imem[5] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 31, 0, 99);
  imem[6] = enc_b(4, 1, 2, 8); imem[7] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 31, 0, 99);
  imem[8] = enc_b(5, 2, 1, 8); imem[9] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 31, 0, 99);
  imem[10] = enc_b(6, 1, 2, 8); imem[11] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 31, 0, 99);
  imem[12] = enc_b(7, 2, 1, 8); imem[13] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 31, 0, 99);
  imem[14] = enc_u(RV32I_TRUE_COMET_OPCODE_AUIPC, 3, 0);
  imem[15] = enc_j(4, 8); imem[16] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 31, 0, 99);
  imem[17] = enc_i(RV32I_TRUE_COMET_OPCODE_JALR, 0, 5, 3, 20); imem[18] = enc_i(RV32I_TRUE_COMET_OPCODE_OP_IMM, 0, 31, 0, 99);
  imem[19] = enc_s(2, 4, 0, 0); imem[20] = enc_s(2, 5, 0, 4); imem[21] = enc_s(2, 31, 0, 8); imem[22] = 0x00000073u;
  int status = rv32i_true_comet_run(imem, dmem);
  return status == RV32I_TRUE_COMET_STATUS_ECALL &&
         dmem_word(dmem, 0) == 64u && dmem_word(dmem, 1) == 72u && dmem_word(dmem, 2) == 0u;
}

/* Check that each architecturally visible termination condition has its own status. */
static bool check_terminal_statuses(void) {
  uint32_t imem[RV32I_TRUE_COMET_IMEM_WORDS] = {};
  uint8_t dmem[RV32I_TRUE_COMET_DMEM_BYTES] = {};
  imem[0] = 0x00100073u;
  if (rv32i_true_comet_run(imem, dmem) != RV32I_TRUE_COMET_STATUS_EBREAK) return false;
  imem[0] = 0xffffffffu;
  if (rv32i_true_comet_run(imem, dmem) != RV32I_TRUE_COMET_STATUS_ILLEGAL_INSTRUCTION) return false;
  imem[0] = enc_i(RV32I_TRUE_COMET_OPCODE_LOAD, 2, 1, 0, 1);
  if (rv32i_true_comet_run(imem, dmem) != RV32I_TRUE_COMET_STATUS_MISALIGNED_DATA) return false;
  imem[0] = enc_b(0, 0, 0, 2);
  if (rv32i_true_comet_run(imem, dmem) != RV32I_TRUE_COMET_STATUS_MISALIGNED_INSTRUCTION) return false;
  imem[0] = 0x1000000fu;
  if (rv32i_true_comet_run(imem, dmem) != RV32I_TRUE_COMET_STATUS_ILLEGAL_INSTRUCTION) return false;
  imem[0] = enc_j(0, RV32I_TRUE_COMET_IMEM_WORDS * 4);
  return rv32i_true_comet_run(imem, dmem) == RV32I_TRUE_COMET_STATUS_INSTRUCTION_ACCESS;
}

/* Report focused benchmark coverage in the stable key=value format used by verify. */
int main(int argc, char **argv) {
  bool arithmetic_and_memory = check_arithmetic_and_memory();
  bool control_flow = check_control_flow();
  bool terminal_statuses = check_terminal_statuses();
  bool ok = arithmetic_and_memory && control_flow && terminal_statuses;
  FILE *out = stdout;
  if (argc > 1) { out = fopen(argv[1], "w"); if (!out) { perror("fopen"); return 1; } }
  fprintf(out, "arithmetic_and_memory=%s\n", arithmetic_and_memory ? "yes" : "no");
  fprintf(out, "control_flow=%s\n", control_flow ? "yes" : "no");
  fprintf(out, "terminal_statuses=%s\n", terminal_statuses ? "yes" : "no");
  fprintf(out, "pass=%s\n", ok ? "yes" : "no");
  if (out != stdout) fclose(out);
  return ok ? 0 : 2;
}
