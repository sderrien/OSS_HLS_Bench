#include <stdbool.h>
#include <stdint.h>

#include "rv32i_true_comet.h"

/*
 * This module is the benchmark's explicit five-stage Comet state transition.
 * Each loop iteration evaluates F->DC, DC->EX, EX->MEM, and MEM->WB from the
 * prior cycle, then commits every record together. This preserves the core's
 * forwarding, load-use interlock, and control-flow squash behavior in C.
 */

typedef struct { uint32_t pc; uint32_t instruction; bool valid; } FtoDC;
typedef struct {
  uint32_t pc, lhs, rhs, store_data, immediate;
  uint8_t opcode, funct3, funct7, rs1, rs2, rd, memory_width;
  bool use_rs1, use_rs2, use_rd, is_load, is_store, load_unsigned;
  bool is_branch, is_jump, valid;
  int terminal_status;
} DCtoEX;
typedef struct {
  uint32_t result, store_data, branch_target;
  uint8_t rd, memory_width;
  bool use_rd, is_load, is_store, load_unsigned, branch_taken, valid;
  int terminal_status;
} EXtoMEM;
typedef struct {
  uint32_t result, address, store_data;
  uint8_t rd, memory_width;
  bool use_rd, is_load, is_store, load_unsigned, valid;
  int terminal_status;
} MEMtoWB;
typedef struct { uint32_t value; uint8_t rd; bool use_rd, valid; } WBOut;

/* Clear one fetch record so a bubble has no instruction-side effects. */
static inline __attribute__((always_inline)) void clear_f_to_dc(FtoDC *value) {
  value->pc = 0; value->instruction = 0; value->valid = false;
}

/* Clear one decode record, including all decoded side-effect controls. */
static inline __attribute__((always_inline)) void clear_dc_to_ex(DCtoEX *value) {
  value->pc = value->lhs = value->rhs = value->store_data = value->immediate = 0;
  value->opcode = value->funct3 = value->funct7 = value->rs1 = value->rs2 = value->rd = 0;
  value->memory_width = 0;
  value->use_rs1 = value->use_rs2 = value->use_rd = value->is_load = value->is_store = false;
  value->load_unsigned = value->is_branch = value->is_jump = value->valid = false;
  value->terminal_status = RV32I_TRUE_COMET_STATUS_ECALL;
}

/* Clear one execute record, ensuring squashed instructions cannot reach memory. */
static inline __attribute__((always_inline)) void clear_ex_to_mem(EXtoMEM *value) {
  value->result = value->store_data = value->branch_target = 0;
  value->rd = value->memory_width = 0;
  value->use_rd = value->is_load = value->is_store = value->load_unsigned = false;
  value->branch_taken = value->valid = false;
  value->terminal_status = RV32I_TRUE_COMET_STATUS_ECALL;
}

/* Clear one memory record, including its deferred terminal outcome. */
static inline __attribute__((always_inline)) void clear_mem_to_wb(MEMtoWB *value) {
  value->result = value->address = value->store_data = 0;
  value->rd = value->memory_width = 0;
  value->use_rd = value->is_load = value->is_store = value->load_unsigned = value->valid = false;
  value->terminal_status = RV32I_TRUE_COMET_STATUS_ECALL;
}

/* Clear the writeback bypass value when no architectural register is written. */
static inline __attribute__((always_inline)) void clear_wb_out(WBOut *value) {
  value->value = 0; value->rd = 0; value->use_rd = value->valid = false;
}

/* Sign extend an unsigned bit field without implementation-defined signed shifts. */
static inline __attribute__((always_inline)) uint32_t sign_extend(uint32_t value, unsigned width) {
  uint32_t sign = 1u << (width - 1u);
  return (value ^ sign) - sign;
}

/* Decode the split S-format immediate used by every RV32I store width. */
static inline __attribute__((always_inline)) uint32_t decode_store_immediate(uint32_t instruction) {
  return sign_extend(((instruction >> 7u) & 0x1fu) | (((instruction >> 25u) & 0x7fu) << 5u), 12);
}

/* Decode the split B-format byte offset used by all conditional branches. */
static inline __attribute__((always_inline)) uint32_t decode_branch_immediate(uint32_t instruction) {
  uint32_t raw = (((instruction >> 31u) & 1u) << 12u) | (((instruction >> 7u) & 1u) << 11u) |
                 (((instruction >> 25u) & 0x3fu) << 5u) | (((instruction >> 8u) & 0xfu) << 1u);
  return sign_extend(raw, 13);
}

/* Decode the split J-format byte offset used by JAL. */
static inline __attribute__((always_inline)) uint32_t decode_jump_immediate(uint32_t instruction) {
  uint32_t raw = (((instruction >> 31u) & 1u) << 20u) | (((instruction >> 12u) & 0xffu) << 12u) |
                 (((instruction >> 20u) & 1u) << 11u) | (((instruction >> 21u) & 0x3ffu) << 1u);
  return sign_extend(raw, 21);
}

/* Mark an otherwise decoded record as an architecturally visible illegal instruction. */
static inline __attribute__((always_inline)) void illegal_instruction(DCtoEX *decoded) {
  decoded->terminal_status = RV32I_TRUE_COMET_STATUS_ILLEGAL_INSTRUCTION;
}

/*
 * Decode exactly the unprivileged RV32I base encodings. Unsupported extension,
 * reserved, and malformed encodings remain valid pipeline records and trap in
 * EX rather than being silently transformed into bubbles.
 */
static inline __attribute__((always_inline)) DCtoEX decode(FtoDC f_to_dc, const uint32_t reg_file[32]) {
  DCtoEX decoded;
  clear_dc_to_ex(&decoded);
  if (!f_to_dc.valid) return decoded;
  uint32_t instruction = f_to_dc.instruction;
  decoded.pc = f_to_dc.pc;
  decoded.opcode = (uint8_t)(instruction & 0x7fu);
  decoded.funct3 = (uint8_t)((instruction >> 12u) & 7u);
  decoded.funct7 = (uint8_t)((instruction >> 25u) & 0x7fu);
  decoded.rs1 = (uint8_t)((instruction >> 15u) & 0x1fu);
  decoded.rs2 = (uint8_t)((instruction >> 20u) & 0x1fu);
  decoded.rd = (uint8_t)((instruction >> 7u) & 0x1fu);
  decoded.lhs = reg_file[decoded.rs1]; decoded.rhs = reg_file[decoded.rs2];
  decoded.store_data = reg_file[decoded.rs2]; decoded.valid = true;

  switch (decoded.opcode) {
  case RV32I_TRUE_COMET_OPCODE_LUI:
    decoded.use_rd = decoded.rd != 0; decoded.immediate = instruction & 0xfffff000u; break;
  case RV32I_TRUE_COMET_OPCODE_AUIPC:
    decoded.use_rd = decoded.rd != 0; decoded.immediate = instruction & 0xfffff000u; break;
  case RV32I_TRUE_COMET_OPCODE_JAL:
    decoded.use_rd = decoded.rd != 0; decoded.is_jump = true; decoded.immediate = decode_jump_immediate(instruction); break;
  case RV32I_TRUE_COMET_OPCODE_JALR:
    if (decoded.funct3 != 0) { illegal_instruction(&decoded); break; }
    decoded.use_rs1 = true; decoded.use_rd = decoded.rd != 0; decoded.is_jump = true;
    decoded.immediate = sign_extend(instruction >> 20u, 12); break;
  case RV32I_TRUE_COMET_OPCODE_BRANCH:
    if (decoded.funct3 != 0 && decoded.funct3 != 1 && decoded.funct3 < 4) { illegal_instruction(&decoded); break; }
    decoded.use_rs1 = decoded.use_rs2 = decoded.is_branch = true;
    decoded.immediate = decode_branch_immediate(instruction); break;
  case RV32I_TRUE_COMET_OPCODE_LOAD:
    if (decoded.funct3 == 0 || decoded.funct3 == 1 || decoded.funct3 == 2 ||
        decoded.funct3 == 4 || decoded.funct3 == 5) {
      decoded.use_rs1 = true; decoded.use_rd = decoded.rd != 0; decoded.is_load = true;
      decoded.memory_width = decoded.funct3 == 0 || decoded.funct3 == 4 ? 1 : (decoded.funct3 == 1 || decoded.funct3 == 5 ? 2 : 4);
      decoded.load_unsigned = decoded.funct3 == 4 || decoded.funct3 == 5;
      decoded.immediate = sign_extend(instruction >> 20u, 12);
    } else illegal_instruction(&decoded);
    break;
  case RV32I_TRUE_COMET_OPCODE_STORE:
    if (decoded.funct3 <= 2) {
      decoded.use_rs1 = decoded.use_rs2 = decoded.is_store = true;
      decoded.memory_width = decoded.funct3 == 0 ? 1 : (decoded.funct3 == 1 ? 2 : 4);
      decoded.immediate = decode_store_immediate(instruction);
    } else illegal_instruction(&decoded);
    break;
  case RV32I_TRUE_COMET_OPCODE_OP_IMM:
    decoded.use_rs1 = true; decoded.use_rd = decoded.rd != 0;
    if (decoded.funct3 == 1) {
      if (decoded.funct7 != 0) illegal_instruction(&decoded); else decoded.immediate = (instruction >> 20u) & 31u;
    } else if (decoded.funct3 == 5) {
      if (decoded.funct7 != 0 && decoded.funct7 != 0x20) illegal_instruction(&decoded); else decoded.immediate = (instruction >> 20u) & 31u;
    } else if (decoded.funct3 <= 7) decoded.immediate = sign_extend(instruction >> 20u, 12);
    else illegal_instruction(&decoded);
    break;
  case RV32I_TRUE_COMET_OPCODE_OP:
    decoded.use_rs1 = decoded.use_rs2 = true; decoded.use_rd = decoded.rd != 0;
    if ((decoded.funct3 == 0 && (decoded.funct7 == 0 || decoded.funct7 == 0x20)) ||
        ((decoded.funct3 == 5) && (decoded.funct7 == 0 || decoded.funct7 == 0x20)) ||
        (((decoded.funct3 >= 1 && decoded.funct3 <= 4) || decoded.funct3 >= 6) && decoded.funct7 == 0)) break;
    illegal_instruction(&decoded); break;
  case RV32I_TRUE_COMET_OPCODE_MISC_MEM:
    if (decoded.funct3 != 0 || ((instruction >> 28u) & 0xfu) != 0) illegal_instruction(&decoded);
    break;
  case RV32I_TRUE_COMET_OPCODE_SYSTEM:
    if (instruction == 0x00000073u) decoded.terminal_status = RV32I_TRUE_COMET_STATUS_ECALL;
    else if (instruction == 0x00100073u) decoded.terminal_status = RV32I_TRUE_COMET_STATUS_EBREAK;
    else illegal_instruction(&decoded);
    break;
  default:
    illegal_instruction(&decoded); break;
  }
  return decoded;
}

/* Return whether an access width violates RV32I's naturally aligned data rule. */
static inline __attribute__((always_inline)) bool data_misaligned(uint32_t address, uint8_t width) {
  return (width == 2 && (address & 1u) != 0) || (width == 4 && (address & 3u) != 0);
}

/* Keep fixed benchmark memories explicit: accesses beyond the array trap, never wrap. */
static inline __attribute__((always_inline)) bool data_access_fault(uint32_t address, uint8_t width) {
  return address > RV32I_TRUE_COMET_DMEM_WORDS * 4u - width;
}

/*
 * Execute an old DC->EX record. Terminal instructions and misalignment traps
 * are carried with the record so older MEM/WB work drains before the function
 * returns, while all younger work is squashed by do_cycle.
 */
static inline __attribute__((always_inline)) EXtoMEM execute(DCtoEX dc_to_ex) {
  EXtoMEM executed;
  clear_ex_to_mem(&executed);
  if (!dc_to_ex.valid) return executed;
  executed.rd = dc_to_ex.rd; executed.use_rd = dc_to_ex.use_rd; executed.is_load = dc_to_ex.is_load;
  executed.is_store = dc_to_ex.is_store; executed.load_unsigned = dc_to_ex.load_unsigned;
  executed.memory_width = dc_to_ex.memory_width; executed.store_data = dc_to_ex.store_data;
  executed.valid = true; executed.terminal_status = dc_to_ex.terminal_status;
  if (executed.terminal_status != RV32I_TRUE_COMET_STATUS_ECALL || dc_to_ex.opcode == RV32I_TRUE_COMET_OPCODE_SYSTEM) return executed;
  if (dc_to_ex.is_load || dc_to_ex.is_store) {
    executed.result = dc_to_ex.lhs + dc_to_ex.immediate;
    if (data_misaligned(executed.result, dc_to_ex.memory_width)) executed.terminal_status = RV32I_TRUE_COMET_STATUS_MISALIGNED_DATA;
    else if (data_access_fault(executed.result, dc_to_ex.memory_width)) executed.terminal_status = dc_to_ex.is_load ? RV32I_TRUE_COMET_STATUS_LOAD_ACCESS : RV32I_TRUE_COMET_STATUS_STORE_ACCESS;
  } else if (dc_to_ex.is_branch) {
    bool equal = dc_to_ex.lhs == dc_to_ex.rhs;
    bool signed_less = (int32_t)dc_to_ex.lhs < (int32_t)dc_to_ex.rhs;
    bool unsigned_less = dc_to_ex.lhs < dc_to_ex.rhs;
    executed.branch_taken = (dc_to_ex.funct3 == 0 && equal) || (dc_to_ex.funct3 == 1 && !equal) ||
                            (dc_to_ex.funct3 == 4 && signed_less) || (dc_to_ex.funct3 == 5 && !signed_less) ||
                            (dc_to_ex.funct3 == 6 && unsigned_less) || (dc_to_ex.funct3 == 7 && !unsigned_less);
    executed.branch_target = dc_to_ex.pc + dc_to_ex.immediate;
    if (executed.branch_taken && (executed.branch_target & 3u) != 0) executed.terminal_status = RV32I_TRUE_COMET_STATUS_MISALIGNED_INSTRUCTION;
  } else if (dc_to_ex.is_jump) {
    executed.result = dc_to_ex.pc + 4u;
    executed.branch_taken = true;
    executed.branch_target = dc_to_ex.opcode == RV32I_TRUE_COMET_OPCODE_JALR ? (dc_to_ex.lhs + dc_to_ex.immediate) & ~1u : dc_to_ex.pc + dc_to_ex.immediate;
    if ((executed.branch_target & 3u) != 0) executed.terminal_status = RV32I_TRUE_COMET_STATUS_MISALIGNED_INSTRUCTION;
  } else if (dc_to_ex.opcode == RV32I_TRUE_COMET_OPCODE_LUI) executed.result = dc_to_ex.immediate;
  else if (dc_to_ex.opcode == RV32I_TRUE_COMET_OPCODE_AUIPC) executed.result = dc_to_ex.pc + dc_to_ex.immediate;
  else if (dc_to_ex.opcode == RV32I_TRUE_COMET_OPCODE_OP_IMM) {
    switch (dc_to_ex.funct3) {
    case 0: executed.result = dc_to_ex.lhs + dc_to_ex.immediate; break;
    case 1: executed.result = dc_to_ex.lhs << dc_to_ex.immediate; break;
    case 2: executed.result = (int32_t)dc_to_ex.lhs < (int32_t)dc_to_ex.immediate; break;
    case 3: executed.result = dc_to_ex.lhs < dc_to_ex.immediate; break;
    case 4: executed.result = dc_to_ex.lhs ^ dc_to_ex.immediate; break;
    case 5: executed.result = dc_to_ex.funct7 == 0x20 ? (uint32_t)((int32_t)dc_to_ex.lhs >> dc_to_ex.immediate) : dc_to_ex.lhs >> dc_to_ex.immediate; break;
    case 6: executed.result = dc_to_ex.lhs | dc_to_ex.immediate; break;
    default: executed.result = dc_to_ex.lhs & dc_to_ex.immediate; break;
    }
  } else if (dc_to_ex.opcode == RV32I_TRUE_COMET_OPCODE_OP) {
    switch (dc_to_ex.funct3) {
    case 0: executed.result = dc_to_ex.funct7 == 0x20 ? dc_to_ex.lhs - dc_to_ex.rhs : dc_to_ex.lhs + dc_to_ex.rhs; break;
    case 1: executed.result = dc_to_ex.lhs << (dc_to_ex.rhs & 31u); break;
    case 2: executed.result = (int32_t)dc_to_ex.lhs < (int32_t)dc_to_ex.rhs; break;
    case 3: executed.result = dc_to_ex.lhs < dc_to_ex.rhs; break;
    case 4: executed.result = dc_to_ex.lhs ^ dc_to_ex.rhs; break;
    case 5: executed.result = dc_to_ex.funct7 == 0x20 ? (uint32_t)((int32_t)dc_to_ex.lhs >> (dc_to_ex.rhs & 31u)) : dc_to_ex.lhs >> (dc_to_ex.rhs & 31u); break;
    case 6: executed.result = dc_to_ex.lhs | dc_to_ex.rhs; break;
    default: executed.result = dc_to_ex.lhs & dc_to_ex.rhs; break;
    }
  }
  return executed;
}

/*
 * Read one naturally aligned RV32I value from the benchmark's byte-addressed
 * little-endian data memory.
 *
 * OpenHLS intentionally retains the variable-width memcpy form: its packed
 * BRAM lowering recognizes this operation and emits the required byte-enable
 * transaction. The other implementations select explicit byte cases through
 * their respective compiler defines, with the portable fallback sharing the
 * same byte-level behavior for native testing.
 */
/* Assemble one width-selected little-endian value without variable-size copies. */
static inline __attribute__((always_inline)) uint32_t load_data_explicit(
    uint8_t data_mem[RV32I_TRUE_COMET_DMEM_BYTES], uint32_t address, uint8_t width) {
  uint32_t raw;
  switch (width) {
  case 1:
    raw = data_mem[address];
    break;
  case 2:
    raw = (uint32_t)data_mem[address] | ((uint32_t)data_mem[address + 1u] << 8u);
    break;
  default:
    raw = (uint32_t)data_mem[address] | ((uint32_t)data_mem[address + 1u] << 8u) |
          ((uint32_t)data_mem[address + 2u] << 16u) | ((uint32_t)data_mem[address + 3u] << 24u);
    break;
  }
  return raw;
}

static inline __attribute__((always_inline)) uint32_t load_data(
    uint8_t data_mem[RV32I_TRUE_COMET_DMEM_BYTES],
    uint32_t address, uint8_t width, bool unsigned_load) {
#ifdef OPENHLS
  uint32_t raw = 0;
  __builtin_memcpy(&raw, &data_mem[address], width);
#elif defined(VITIS)
  uint32_t raw = load_data_explicit(data_mem, address, width);
#elif defined(BAMBU)
  uint32_t raw = load_data_explicit(data_mem, address, width);
#elif defined(LEGUP)
  uint32_t raw = load_data_explicit(data_mem, address, width);
#else
  uint32_t raw = load_data_explicit(data_mem, address, width);
#endif
  return unsigned_load || width == 4 ? raw : sign_extend(raw, width * 8u);
}

/*
 * Write one naturally aligned RV32I value through the selected byte-enabled
 * memory implementation. The cases mirror load_data so every tool observes the
 * same little-endian byte, halfword, and word state.
 */
/* Scatter one width-selected little-endian value through explicit byte writes. */
static inline __attribute__((always_inline)) void store_data_explicit(
    uint8_t data_mem[RV32I_TRUE_COMET_DMEM_BYTES], uint32_t address, uint8_t width, uint32_t value) {
  switch (width) {
  case 1:
    data_mem[address] = (uint8_t)value;
    break;
  case 2:
    data_mem[address] = (uint8_t)value;
    data_mem[address + 1u] = (uint8_t)(value >> 8u);
    break;
  default:
    data_mem[address] = (uint8_t)value;
    data_mem[address + 1u] = (uint8_t)(value >> 8u);
    data_mem[address + 2u] = (uint8_t)(value >> 16u);
    data_mem[address + 3u] = (uint8_t)(value >> 24u);
    break;
  }
}

static inline __attribute__((always_inline)) void store_data(
    uint8_t data_mem[RV32I_TRUE_COMET_DMEM_BYTES],
    uint32_t address, uint8_t width, uint32_t value) {
#ifdef OPENHLS
  __builtin_memcpy(&data_mem[address], &value, width);
#elif defined(VITIS)
  store_data_explicit(data_mem, address, width, value);
#elif defined(BAMBU)
  store_data_explicit(data_mem, address, width, value);
#elif defined(LEGUP)
  store_data_explicit(data_mem, address, width, value);
#else
  store_data_explicit(data_mem, address, width, value);
#endif
}

/* Convert an old EX->MEM record into MEM->WB and perform a zero-wait banked load. */
static inline __attribute__((always_inline)) MEMtoWB memory_stage(
    EXtoMEM ex_to_mem, uint8_t data_mem[RV32I_TRUE_COMET_DMEM_BYTES]) {
  MEMtoWB memory;
  clear_mem_to_wb(&memory);
  if (!ex_to_mem.valid) return memory;
  memory.result = ex_to_mem.result; memory.address = ex_to_mem.result; memory.store_data = ex_to_mem.store_data;
  memory.rd = ex_to_mem.rd; memory.memory_width = ex_to_mem.memory_width; memory.use_rd = ex_to_mem.use_rd;
  memory.is_load = ex_to_mem.is_load; memory.is_store = ex_to_mem.is_store; memory.load_unsigned = ex_to_mem.load_unsigned;
  memory.valid = true; memory.terminal_status = ex_to_mem.terminal_status;
  if (memory.terminal_status == RV32I_TRUE_COMET_STATUS_ECALL && memory.is_load) memory.result = load_data(data_mem, memory.address, memory.memory_width, memory.load_unsigned);
  return memory;
}

/* Materialize a committing register value for writeback and same-cycle bypassing. */
static inline __attribute__((always_inline)) WBOut writeback(MEMtoWB mem_to_wb) {
  WBOut wb;
  clear_wb_out(&wb);
  if (mem_to_wb.valid && mem_to_wb.terminal_status == RV32I_TRUE_COMET_STATUS_ECALL && mem_to_wb.use_rd && mem_to_wb.rd != 0) {
    wb.value = mem_to_wb.result; wb.rd = mem_to_wb.rd; wb.use_rd = wb.valid = true;
  }
  return wb;
}

/* Identify the one-cycle interlock required before a load value reaches MEM bypass. */
static inline __attribute__((always_inline)) bool load_use_stall(DCtoEX execute_stage, DCtoEX decoded) {
  return execute_stage.valid && execute_stage.is_load && execute_stage.use_rd &&
         ((decoded.use_rs1 && decoded.rs1 == execute_stage.rd) || (decoded.use_rs2 && decoded.rs2 == execute_stage.rd));
}

/* Select the newest available Comet operand producer in EX, MEM, then WB order. */
static inline __attribute__((always_inline)) uint32_t forwarded_operand(uint32_t original, uint8_t source_reg, EXtoMEM execute_stage, MEMtoWB memory_stage_value, WBOut writeback_value) {
  if (source_reg == 0) return 0;
  if (execute_stage.valid && execute_stage.terminal_status == RV32I_TRUE_COMET_STATUS_ECALL && execute_stage.use_rd && !execute_stage.is_load && execute_stage.rd == source_reg) return execute_stage.result;
  if (memory_stage_value.valid && memory_stage_value.terminal_status == RV32I_TRUE_COMET_STATUS_ECALL && memory_stage_value.use_rd && memory_stage_value.rd == source_reg) return memory_stage_value.result;
  if (writeback_value.valid && writeback_value.rd == source_reg) return writeback_value.value;
  return original;
}

/* Return whether all pipeline records are bubbles after a terminal event drains. */
static inline __attribute__((always_inline)) bool pipeline_empty(FtoDC f_to_dc, DCtoEX dc_to_ex, EXtoMEM ex_to_mem, MEMtoWB mem_to_wb) {
  return !f_to_dc.valid && !dc_to_ex.valid && !ex_to_mem.valid && !mem_to_wb.valid;
}

/*
 * Advance every Comet stage by one clock. A terminal EX record squashes only
 * younger work, lets older memory/writeback records retire, and then prevents
 * further fetches. This is the cycle-accurate exception boundary for the core.
 */
static inline __attribute__((always_inline)) void do_cycle(uint32_t reg_file[32], uint32_t *pc, bool *fetch_enabled, bool *terminal_seen, int *status, FtoDC *f_to_dc, DCtoEX *dc_to_ex, EXtoMEM *ex_to_mem, MEMtoWB *mem_to_wb, uint32_t instr_mem[RV32I_TRUE_COMET_IMEM_WORDS], uint8_t data_mem[RV32I_TRUE_COMET_DMEM_BYTES]) {
  FtoDC fetched; clear_f_to_dc(&fetched);
  if (*fetch_enabled) {
    if ((*pc & 3u) != 0) { *status = RV32I_TRUE_COMET_STATUS_MISALIGNED_INSTRUCTION; *terminal_seen = true; *fetch_enabled = false; }
    else if (*pc >= RV32I_TRUE_COMET_IMEM_WORDS * 4u) { *status = RV32I_TRUE_COMET_STATUS_INSTRUCTION_ACCESS; *terminal_seen = true; *fetch_enabled = false; }
    else { fetched.pc = *pc; fetched.instruction = instr_mem[*pc >> 2u]; fetched.valid = true; }
  }
  DCtoEX decoded = decode(*f_to_dc, reg_file);
  EXtoMEM executed = execute(*dc_to_ex);
  MEMtoWB memory = memory_stage(*ex_to_mem, data_mem);
  WBOut wb = writeback(*mem_to_wb);
  bool terminal = executed.valid && (executed.terminal_status != RV32I_TRUE_COMET_STATUS_ECALL || dc_to_ex->opcode == RV32I_TRUE_COMET_OPCODE_SYSTEM);
  bool redirect = executed.valid && executed.terminal_status == RV32I_TRUE_COMET_STATUS_ECALL && executed.branch_taken;
  bool decode_stall = !terminal && !redirect && load_use_stall(*dc_to_ex, decoded);
  if (decoded.valid && !decode_stall && !terminal && !redirect) {
    decoded.lhs = forwarded_operand(decoded.lhs, decoded.rs1, executed, memory, wb);
    decoded.rhs = forwarded_operand(decoded.rhs, decoded.rs2, executed, memory, wb);
    decoded.store_data = forwarded_operand(decoded.store_data, decoded.rs2, executed, memory, wb);
  }
  if (ex_to_mem->valid && ex_to_mem->terminal_status == RV32I_TRUE_COMET_STATUS_ECALL && ex_to_mem->is_store) store_data(data_mem, ex_to_mem->result, ex_to_mem->memory_width, ex_to_mem->store_data);
  if (wb.valid) reg_file[wb.rd] = wb.value;
  *mem_to_wb = memory; *ex_to_mem = executed;
  if (terminal || redirect || decode_stall) clear_dc_to_ex(dc_to_ex); else *dc_to_ex = decoded;
  if (terminal) {
    *status = executed.terminal_status; *terminal_seen = true; *fetch_enabled = false; clear_f_to_dc(f_to_dc);
  } else if (redirect) {
    *pc = executed.branch_target; clear_f_to_dc(f_to_dc);
  } else if (!decode_stall) {
    *f_to_dc = fetched;
    if (*fetch_enabled) *pc += 4u;
  }
}

/*
 * Execute the finite benchmark harness. The bound protects a program without
 * a terminal instruction; correct programs end with ECALL after prior stages
 * have drained, and return a precise stop or trap status.
 */
#pragma HLS toplevel
int rv32i_true_comet_run(uint32_t instr_mem[RV32I_TRUE_COMET_IMEM_WORDS], uint8_t data_mem[RV32I_TRUE_COMET_DMEM_BYTES]) {
  #pragma HLS interface mode=bram port=data_mem element_width=8 bram_data_width=32
  #pragma HLS resource variable=reg_file core=ram_2p read_ports=2 write_ports=1
  uint32_t reg_file[32] = {0}; uint32_t pc = 0; bool fetch_enabled = true; bool terminal_seen = false;
  int status = RV32I_TRUE_COMET_STATUS_TIMEOUT;
  FtoDC f_to_dc; DCtoEX dc_to_ex; EXtoMEM ex_to_mem; MEMtoWB mem_to_wb;
  clear_f_to_dc(&f_to_dc); clear_dc_to_ex(&dc_to_ex); clear_ex_to_mem(&ex_to_mem); clear_mem_to_wb(&mem_to_wb);

  /*
   * Age the finite-harness stop token through four loop-carried registers.
   *
   * Role:
   * Keep the architectural 64-cycle execution limit while making the loop
   * continue consumer depend on a token produced four logical iterations
   * earlier. `run_core_cycle` remains the immediate budget decision, so drain
   * iterations cannot execute a 65th Comet cycle or introduce side effects.
   *
   * Goal:
   * Exercise an II=1 control recurrence with an explicit aging window rather
   * than requiring the scheduler to resolve the current iteration's loop-exit
   * predicate before launching its successor.
   */
  bool budget_age0 = true, budget_age1 = true, budget_age2 = true, budget_age3 = true;
  #pragma HLS pipeline ii=1
  for (int cycle = 0; budget_age3; ++cycle) {
    bool run_core_cycle = cycle < RV32I_TRUE_COMET_MAX_CYCLES;
    budget_age3 = budget_age2;
    budget_age2 = budget_age1;
    budget_age1 = budget_age0;
    budget_age0 = run_core_cycle;
    if (run_core_cycle) {
      do_cycle(reg_file, &pc, &fetch_enabled, &terminal_seen, &status, &f_to_dc, &dc_to_ex, &ex_to_mem, &mem_to_wb, instr_mem, data_mem);
    }
  }
  return status;
}
