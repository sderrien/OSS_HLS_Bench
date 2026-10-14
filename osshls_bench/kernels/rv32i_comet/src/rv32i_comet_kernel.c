#include "rv32i_comet.h"

#include <stdbool.h>
#include <stdio.h>

void __hls_pipeline_marker(int, int, int);

static __attribute__((always_inline)) int32_t sign_extend(uint32_t value, unsigned bits) {
  uint32_t sign = 1u << (bits - 1u);
  return (int32_t)((value ^ sign) - sign);
}

static __attribute__((always_inline)) int32_t imm_i(uint32_t instr) { return sign_extend(instr >> 20, 12); }

static __attribute__((always_inline)) int32_t imm_s(uint32_t instr) {
  uint32_t imm = ((instr >> 7) & 0x1fu) | (((instr >> 25) & 0x7fu) << 5);
  return sign_extend(imm, 12);
}

static __attribute__((always_inline)) int32_t imm_b(uint32_t instr) {
  uint32_t imm = (((instr >> 8) & 0x0fu) << 1) |
                 (((instr >> 25) & 0x3fu) << 5) |
                 (((instr >> 7) & 0x01u) << 11) |
                 (((instr >> 31) & 0x01u) << 12);
  return sign_extend(imm, 13);
}

static __attribute__((always_inline)) bool ifid_reads_rd(uint32_t instr, bool valid, uint8_t rd) {
  if (!valid || rd == 0) {
    return false;
  }
  uint8_t opcode = (uint8_t)(instr & 0x7fu);
  uint8_t funct3 = (uint8_t)((instr >> 12) & 0x07u);
  uint8_t funct7 = (uint8_t)((instr >> 25) & 0x7fu);
  uint8_t rs1 = (uint8_t)((instr >> 15) & 0x1fu);
  uint8_t rs2 = (uint8_t)((instr >> 20) & 0x1fu);
  bool use_rs1 = false;
  bool use_rs2 = false;

  if (opcode == RV32I_OPCODE_OP_IMM && funct3 == RV32I_FUNCT3_ADDI) {
    use_rs1 = true;
  } else if (opcode == RV32I_OPCODE_OP && funct3 == RV32I_FUNCT3_ADD_SUB &&
             funct7 == RV32I_FUNCT7_SUB) {
    use_rs1 = true;
    use_rs2 = true;
  } else if (opcode == RV32I_OPCODE_LOAD && funct3 == RV32I_FUNCT3_LW) {
    use_rs1 = true;
  } else if (opcode == RV32I_OPCODE_STORE && funct3 == RV32I_FUNCT3_SW) {
    use_rs1 = true;
    use_rs2 = true;
  } else if (opcode == RV32I_OPCODE_BRANCH && funct3 == RV32I_FUNCT3_BNE) {
    use_rs1 = true;
    use_rs2 = true;
  }

  return (use_rs1 && rs1 == rd) || (use_rs2 && rs2 == rd);
}

// Read a RISC-V integer register by architectural index; x0 stays hard-wired to zero.
static __attribute__((always_inline)) uint32_t read_reg(uint8_t idx, const uint32_t x[8]) {
  return idx < 8 ? x[idx] : 0;
}

#pragma HLS toplevel
// Top-level RV32I comet kernel: runs the in-order pipeline model over instruction and data memories.
int rv32i_comet_run(uint32_t instr_mem[RV32I_COMET_IMEM_WORDS],
                    uint32_t data_mem[RV32I_COMET_DMEM_WORDS]) {
  uint32_t x[8] = {0};

  // Local instruction/data memories decouple the pipelined core model from the
  // external memory request/ack protocol.  The simulation loop below models the
  // processor pipeline against these local arrays; external memories are only
  // used for copy-in before the loop and copy-out after the loop.



  uint32_t ifid_pc = 0;
  uint32_t ifid_instr = 0;
  bool ifid_valid = false;

  uint32_t idex_pc = 0;
  uint32_t idex_instr = 0;
  uint8_t idex_opcode = 0;
  uint8_t idex_funct3 = 0;
  uint8_t idex_funct7 = 0;
  uint8_t idex_rd = 0;
  uint8_t idex_rs1 = 0;
  uint8_t idex_rs2 = 0;
  uint32_t idex_rs1_value = 0;
  uint32_t idex_rs2_value = 0;
  int32_t idex_imm = 0;
  bool idex_use_rs1 = false;
  bool idex_use_rs2 = false;
  bool idex_use_rd = false;
  bool idex_is_load = false;
  bool idex_is_store = false;
  bool idex_is_branch = false;
  bool idex_is_sub = false;
  bool idex_valid = false;

  uint8_t exmem_rd = 0;
  uint32_t exmem_result = 0;
  uint32_t exmem_store_value = 0;
  uint32_t exmem_branch_target = 0;
  bool exmem_branch_taken = false;
  bool exmem_use_rd = false;
  bool exmem_is_load = false;
  bool exmem_is_store = false;
  bool exmem_valid = false;

  uint8_t memwb_rd = 0;
  uint32_t memwb_value = 0;
  bool memwb_use_rd = false;
  bool memwb_valid = false;

  uint32_t pc = 0;
  bool fetch_enable = true;

#pragma HLS pipeline
  for (int cycle = 0; cycle < RV32I_COMET_MAX_CYCLES; ++cycle) {
    // WB: retire the oldest in-flight result into the architectural register file.
    if (memwb_valid && memwb_use_rd && memwb_rd != 0) {
      x[memwb_rd] = memwb_value;
    }

    // MEM: resolve load/store side effects and prepare the value forwarded to WB.
    uint8_t mem_next_rd = 0;
    uint32_t mem_next_value = 0;
    bool mem_next_use_rd = false;
    bool mem_next_valid = false;
    if (exmem_valid) {
      mem_next_rd = exmem_rd;
      mem_next_use_rd = exmem_use_rd;
      mem_next_valid = exmem_use_rd || exmem_is_load;
      if (exmem_is_load) {
        uint32_t mem_addr = exmem_result >> 2;
        mem_next_value = data_mem[mem_addr & (RV32I_COMET_DMEM_WORDS - 1)];
      } else if (exmem_is_store) {
        uint32_t mem_addr = exmem_result >> 2;
        data_mem[mem_addr & (RV32I_COMET_DMEM_WORDS - 1)] = exmem_store_value;
        mem_next_valid = false;
      } else {
        mem_next_value = exmem_result;
      }
    }

    // EX: execute with bypasses from EX/MEM and MEM/WB when the source register matches.
    uint32_t lhs = idex_rs1_value;
    uint32_t rhs = idex_rs2_value;
    if (idex_valid && idex_use_rs1 && exmem_valid && exmem_use_rd && !exmem_is_load &&
        exmem_rd != 0 && exmem_rd == idex_rs1) {
      lhs = exmem_result;
    } else if (idex_valid && idex_use_rs1 && memwb_valid && memwb_use_rd && memwb_rd != 0 &&
               memwb_rd == idex_rs1) {
      lhs = memwb_value;
    }
    if (idex_valid && idex_use_rs2 && exmem_valid && exmem_use_rd && !exmem_is_load &&
        exmem_rd != 0 && exmem_rd == idex_rs2) {
      rhs = exmem_result;
    } else if (idex_valid && idex_use_rs2 && memwb_valid && memwb_use_rd && memwb_rd != 0 &&
               memwb_rd == idex_rs2) {
      rhs = memwb_value;
    }

    uint8_t ex_next_rd = 0;
    uint32_t ex_next_result = 0;
    uint32_t ex_next_store_value = 0;
    uint32_t ex_next_branch_target = 0;
    bool ex_next_branch_taken = false;
    bool ex_next_use_rd = false;
    bool ex_next_is_load = false;
    bool ex_next_is_store = false;
    bool ex_next_valid = false;
    if (idex_valid) {
      ex_next_rd = idex_rd;
      ex_next_use_rd = idex_use_rd;
      ex_next_is_load = idex_is_load;
      ex_next_is_store = idex_is_store;
      ex_next_valid = true;
      ex_next_store_value = rhs;
      if (idex_is_load || idex_is_store) {
        ex_next_result = lhs + (uint32_t)idex_imm;
      } else if (idex_is_sub) {
        ex_next_result = lhs - rhs;
      } else if (idex_is_branch) {
        ex_next_branch_taken = lhs != rhs;
        ex_next_branch_target = idex_pc + (uint32_t)idex_imm;
      } else {
        ex_next_result = lhs + (uint32_t)idex_imm;
      }
    }

    // DC: decode the fetched instruction and detect RAW/load-use stalls before launch.
    uint8_t dc_opcode = (uint8_t)(ifid_instr & 0x7fu);
    uint8_t dc_funct3 = (uint8_t)((ifid_instr >> 12) & 0x07u);
    uint8_t dc_funct7 = (uint8_t)((ifid_instr >> 25) & 0x7fu);
    uint8_t dc_rd = (uint8_t)((ifid_instr >> 7) & 0x1fu);
    uint8_t dc_rs1 = (uint8_t)((ifid_instr >> 15) & 0x1fu);
    uint8_t dc_rs2 = (uint8_t)((ifid_instr >> 20) & 0x1fu);
    int32_t dc_imm = 0;
    bool dc_use_rs1 = false;
    bool dc_use_rs2 = false;
    bool dc_use_rd = false;
    bool dc_is_load = false;
    bool dc_is_store = false;
    bool dc_is_branch = false;
    bool dc_is_sub = false;
    bool dc_valid = ifid_valid;

    if (ifid_valid && dc_opcode == RV32I_OPCODE_OP_IMM && dc_funct3 == RV32I_FUNCT3_ADDI) {
      dc_imm = imm_i(ifid_instr);
      dc_use_rs1 = true;
      dc_use_rd = dc_rd != 0;
    } else if (ifid_valid && dc_opcode == RV32I_OPCODE_OP && dc_funct3 == RV32I_FUNCT3_ADD_SUB &&
               dc_funct7 == RV32I_FUNCT7_SUB) {
      dc_use_rs1 = true;
      dc_use_rs2 = true;
      dc_use_rd = dc_rd != 0;
      dc_is_sub = true;
    } else if (ifid_valid && dc_opcode == RV32I_OPCODE_LOAD && dc_funct3 == RV32I_FUNCT3_LW) {
      dc_imm = imm_i(ifid_instr);
      dc_use_rs1 = true;
      dc_use_rd = dc_rd != 0;
      dc_is_load = true;
    } else if (ifid_valid && dc_opcode == RV32I_OPCODE_STORE && dc_funct3 == RV32I_FUNCT3_SW) {
      dc_imm = imm_s(ifid_instr);
      dc_use_rs1 = true;
      dc_use_rs2 = true;
      dc_is_store = true;
    } else if (ifid_valid && dc_opcode == RV32I_OPCODE_BRANCH && dc_funct3 == RV32I_FUNCT3_BNE) {
      dc_imm = imm_b(ifid_instr);
      dc_use_rs1 = true;
      dc_use_rs2 = true;
      dc_is_branch = true;
    } else {
      dc_valid = false;
    }

    bool dc_load_stall = idex_valid && idex_is_load && idex_use_rd &&
                         ifid_reads_rd(ifid_instr, ifid_valid, idex_rd);

    // F: fetch the next instruction word unless a branch flush or load stall blocks launch.
    uint32_t f_fetch_word_index = pc >> 2;
    uint32_t f_fetched = instr_mem[f_fetch_word_index & (RV32I_COMET_IMEM_WORDS - 1)];

    bool ex_branch_taken = ex_next_valid && ex_next_branch_taken;
    uint32_t ex_branch_target = ex_next_branch_target;

    if (ex_branch_taken) {
      pc = ex_branch_target;
      ifid_valid = false;
      idex_valid = false;
      fetch_enable = true;
    } else if (dc_load_stall) {
      idex_valid = false;
    } else {
      idex_pc = ifid_pc;
      idex_instr = ifid_instr;
      idex_opcode = dc_opcode;
      idex_funct3 = dc_funct3;
      idex_funct7 = dc_funct7;
      idex_rd = dc_rd;
      idex_rs1 = dc_rs1;
      idex_rs2 = dc_rs2;
      idex_rs1_value = read_reg(dc_rs1, x);
      idex_rs2_value = read_reg(dc_rs2, x);
      idex_imm = dc_imm;
      idex_use_rs1 = dc_use_rs1;
      idex_use_rs2 = dc_use_rs2;
      idex_use_rd = dc_use_rd;
      idex_is_load = dc_is_load;
      idex_is_store = dc_is_store;
      idex_is_branch = dc_is_branch;
      idex_is_sub = dc_is_sub;
      idex_valid = dc_valid;

      if (fetch_enable) {
        ifid_pc = pc;
        ifid_instr = f_fetched;
        ifid_valid = f_fetched != 0;
        pc = pc + 4;
        if (f_fetched == 0) {
          fetch_enable = false;
        }
      } else {
        ifid_valid = false;
      }
    }

    // Logging preserves the stage naming to make the 5-stage structure explicit.
    if (idex_valid) {
      if (idex_opcode == RV32I_OPCODE_OP_IMM && idex_funct3 == RV32I_FUNCT3_ADDI) {
        printf("[rv32i-comet] cycle=%d stage=EX asm='addi x%u, x%u, %d' effect='x[%u]=x[%u]+%d -> %u'\n",
               cycle, (unsigned)idex_rd, (unsigned)idex_rs1, (int)idex_imm,
               (unsigned)idex_rd, (unsigned)idex_rs1, (int)idex_imm, ex_next_result);
      } else if (idex_opcode == RV32I_OPCODE_OP && idex_funct3 == RV32I_FUNCT3_ADD_SUB &&
                 idex_funct7 == RV32I_FUNCT7_SUB) {
        printf("[rv32i-comet] cycle=%d stage=EX asm='sub x%u, x%u, x%u' effect='x[%u]=x[%u]-x[%u] -> %u'\n",
               cycle, (unsigned)idex_rd, (unsigned)idex_rs1, (unsigned)idex_rs2,
               (unsigned)idex_rd, (unsigned)idex_rs1, (unsigned)idex_rs2, ex_next_result);
      } else if (idex_opcode == RV32I_OPCODE_LOAD && idex_funct3 == RV32I_FUNCT3_LW) {
        uint32_t byte_addr = ex_next_result;
        uint32_t word_addr = (byte_addr >> 2) & (RV32I_COMET_DMEM_WORDS - 1);
        printf("[rv32i-comet] cycle=%d stage=EX asm='lw x%u, %d(x%u)' effect='x[%u]=mem32[%u] byte_addr=%u'\n",
               cycle, (unsigned)idex_rd, (int)idex_imm, (unsigned)idex_rs1,
               (unsigned)idex_rd, word_addr, byte_addr);
      } else if (idex_opcode == RV32I_OPCODE_STORE && idex_funct3 == RV32I_FUNCT3_SW) {
        uint32_t byte_addr = ex_next_result;
        uint32_t word_addr = (byte_addr >> 2) & (RV32I_COMET_DMEM_WORDS - 1);
        printf("[rv32i-comet] cycle=%d stage=EX asm='sw x%u, %d(x%u)' effect='mem32[%u]=x[%u] -> %u byte_addr=%u'\n",
               cycle, (unsigned)idex_rs2, (int)idex_imm, (unsigned)idex_rs1,
               word_addr, (unsigned)idex_rs2, ex_next_store_value, byte_addr);
      } else if (idex_opcode == RV32I_OPCODE_BRANCH && idex_funct3 == RV32I_FUNCT3_BNE) {
        printf("[rv32i-comet] cycle=%d stage=EX asm='bne x%u, x%u, %d' effect='pc=(x[%u]=%u != x[%u]=%u) ? 0x%08x : 0x%08x -> %s'\n",
               cycle, (unsigned)idex_rs1, (unsigned)idex_rs2, (int)idex_imm,
               (unsigned)idex_rs1, lhs, (unsigned)idex_rs2, rhs,
               ex_next_branch_target, idex_pc + 4u, ex_next_branch_taken ? "taken" : "not-taken");
      } else {
        printf("[rv32i-comet] cycle=%d stage=EX asm='unknown' effect='none'\n", cycle);
      }
    } else {
      printf("[rv32i-comet] cycle=%d stage=EX asm='bubble' effect='none'\n", cycle);
    }

    if (exmem_valid && exmem_is_load) {
      uint32_t byte_addr = exmem_result;
      uint32_t word_addr = (byte_addr >> 2) & (RV32I_COMET_DMEM_WORDS - 1);
      printf("[rv32i-comet] cycle=%d stage=MEM mem-effect='load mem32[%u]=%u -> x[%u]'\n",
             cycle, word_addr, mem_next_value, (unsigned)exmem_rd);
    }
    if (memwb_valid && memwb_use_rd && memwb_rd != 0) {
      printf("[rv32i-comet] cycle=%d stage=WB wb-effect='x[%u]=%u'\n", cycle,
             (unsigned)memwb_rd, memwb_value);
    }
    if (dc_load_stall) {
      printf("[rv32i-comet] cycle=%d stage=DC control='load-stall idex.rd=x%u holds ifid.pc=0x%08x'\n",
             cycle, (unsigned)idex_rd, ifid_pc);
    }
    if (ex_branch_taken) {
      printf("[rv32i-comet] cycle=%d stage=F control='branch-flush target=0x%08x'\n", cycle,
             ex_branch_target);
    }

    exmem_rd = ex_next_rd;
    exmem_result = ex_next_result;
    exmem_store_value = ex_next_store_value;
    exmem_branch_target = ex_next_branch_target;
    exmem_branch_taken = ex_next_branch_taken;
    exmem_use_rd = ex_next_use_rd;
    exmem_is_load = ex_next_is_load;
    exmem_is_store = ex_next_is_store;
    exmem_valid = ex_next_valid;

    memwb_rd = mem_next_rd;
    memwb_value = mem_next_value;
    memwb_use_rd = mem_next_use_rd;
    memwb_valid = mem_next_valid;
  }

  return (int)(data_mem[0] + data_mem[1]);
}
