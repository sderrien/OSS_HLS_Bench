/*
 * Role:
 * Implement the fixed-memory, synthesizable reference interpreter used by the
 * RV32I benchmark.
 *
 * Goal:
 * Model exactly the unprivileged RV32I integer instruction set over the
 * benchmark's four byte-lane data memories. The core deliberately excludes
 * M/A/C, CSR, and privileged instructions; FENCE is a no-op because this
 * single-threaded memory harness has no ordering-visible agents.
 *
 * How:
 * Fetches use the pre-existing byte-addressed instruction-memory convention,
 * then decode validates every opcode subencoding before changing architectural
 * state. Stops and alignment traps return the documented negative ABI values.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "mem_image.h"
#include "rv32i.h"

/*
 * Role:
 * Execute RV32I instructions from `init_pc` over the benchmark's fixed
 * instruction and four-lane data-memory interface.
 *
 * Goal:
 * Provide the synthesizable implementation and host reference semantics for
 * all valid base-integer instructions while preserving x0, RV32 wrapping
 * arithmetic, and the historical memory layout.
 *
 * How:
 * The loop validates instruction alignment before each fetch and validates
 * naturally aligned halfword/word accesses before any register or memory
 * update. Valid instructions advance `pc`; ECALL, EBREAK, invalid encodings,
 * and alignment faults return their distinct `RV32I_*` status values.
 */
int cpu_run(uint32_t init_pc, uint32_t instr_mem[MEM_SIZE / 4], uint8_t mem0[MEM_SIZE / 4],
            uint8_t mem1[MEM_SIZE / 4], uint8_t mem2[MEM_SIZE / 4], uint8_t mem3[MEM_SIZE / 4]) {
    uint32_t pc = init_pc;
    uint32_t x[32] = {0};
    x[REGISTER_SP] = MEM_SIZE - 4;

    while (true) {
        if ((pc & 3u) != 0)
            return RV32I_TRAP_INSTRUCTION_ADDRESS_MISALIGNED;

        /* The instruction array is intentionally indexed by byte PC in this fixed harness. */
        if (pc >= (MEM_SIZE / 4))
            return RV32I_TRAP_INSTRUCTION_ACCESS_FAULT;
        uint32_t instr = instr_mem[pc];
        uint8_t opcode = instr & 0x7fu;
        uint8_t rd = (instr >> 7) & 0x1fu;
        uint8_t funct3 = (instr >> 12) & 0x7u;
        uint8_t rs1 = (instr >> 15) & 0x1fu;
        uint8_t rs2 = (instr >> 20) & 0x1fu;
        uint8_t funct7 = instr >> 25;
        uint32_t simm_i = (uint32_t)((int32_t)instr >> 20);
        uint32_t simm_s = (uint32_t)((int32_t)(instr & 0xfe000000u) >> 20) | ((instr >> 7) & 0x1fu);
        /* B-immediates scatter imm[12|10:5|4:1|11] across instruction bits
         * [31|30:25|11:8|7]; preserve bit 7 as branch-offset bit 11. */
        uint32_t simm_b = (uint32_t)((int32_t)(instr & 0x80000000u) >> 19) |
                          ((instr >> 20) & 0x7e0u) | ((instr >> 7) & 0x1eu) |
                          ((instr << 4) & 0x800u);
        uint32_t simm_j = (uint32_t)((int32_t)(instr & 0x80000000u) >> 11) |
                          (instr & 0x000ff000u) | ((instr >> 9) & 0x800u) |
                          ((instr >> 20) & 0x7feu);
        uint32_t addr;
        uint32_t index;
        uint32_t value;

#ifndef NOTRACE
        printf("pc=0x%x;ir=0x%08x ", pc, instr);
#endif
        switch (opcode) {
        case OPCODE_LUI:
            if (rd != 0) x[rd] = instr & 0xfffff000u;
            pc += 4;
            break;
        case OPCODE_AUIPC:
            if (rd != 0) x[rd] = pc + (instr & 0xfffff000u);
            pc += 4;
            break;
        case OPCODE_JAL:
            if (((pc + simm_j) & 3u) != 0) return RV32I_TRAP_INSTRUCTION_ADDRESS_MISALIGNED;
            if (rd != 0) x[rd] = pc + 4;
            pc += simm_j;
            break;
        case OPCODE_JALR:
            if (funct3 != 0) return RV32I_STOP_INVALID_INSTRUCTION;
            value = pc + 4;
            pc = (x[rs1] + simm_i) & ~1u;
            if ((pc & 3u) != 0) return RV32I_TRAP_INSTRUCTION_ADDRESS_MISALIGNED;
            if (rd != 0) x[rd] = value;
            break;
        case OPCODE_BR:
            switch (funct3) {
            case FUNCT3_BEQ: value = x[rs1] == x[rs2]; break;
            case FUNCT3_BNE: value = x[rs1] != x[rs2]; break;
            case FUNCT3_BLT: value = (int32_t)x[rs1] < (int32_t)x[rs2]; break;
            case FUNCT3_BGE: value = (int32_t)x[rs1] >= (int32_t)x[rs2]; break;
            case FUNCT3_BLTU: value = x[rs1] < x[rs2]; break;
            case FUNCT3_BGEU: value = x[rs1] >= x[rs2]; break;
            default: return RV32I_STOP_INVALID_INSTRUCTION;
            }
            pc += value ? simm_b : 4;
            break;
        case OPCODE_LOAD:
            addr = x[rs1] + simm_i;
            index = addr >> 2;
            if (index >= (MEM_SIZE / 4)) return RV32I_TRAP_LOAD_ACCESS_FAULT;
            switch (funct3) {
            case FUNCT3_LB:
            case FUNCT3_LBU:
                value = (addr & 3u) == 0 ? mem0[index] : (addr & 3u) == 1 ? mem1[index] :
                        (addr & 3u) == 2 ? mem2[index] : mem3[index];
                if (funct3 == FUNCT3_LB) value = (uint32_t)(int32_t)(int8_t)value;
                break;
            case FUNCT3_LH:
            case FUNCT3_LHU:
                if ((addr & 1u) != 0) return RV32I_TRAP_LOAD_STORE_ADDRESS_MISALIGNED;
                value = (addr & 2u) == 0 ? (uint32_t)mem0[index] | ((uint32_t)mem1[index] << 8) :
                                           (uint32_t)mem2[index] | ((uint32_t)mem3[index] << 8);
                if (funct3 == FUNCT3_LH) value = (uint32_t)(int32_t)(int16_t)value;
                break;
            case FUNCT3_LW:
                if ((addr & 3u) != 0) return RV32I_TRAP_LOAD_STORE_ADDRESS_MISALIGNED;
                value = (uint32_t)mem0[index] | ((uint32_t)mem1[index] << 8) |
                        ((uint32_t)mem2[index] << 16) | ((uint32_t)mem3[index] << 24);
                break;
            default: return RV32I_STOP_INVALID_INSTRUCTION;
            }
            if (rd != 0) x[rd] = value;
            pc += 4;
            break;
        case OPCODE_STORE:
            addr = x[rs1] + simm_s;
            index = addr >> 2;
            if (index >= (MEM_SIZE / 4)) return RV32I_TRAP_STORE_ACCESS_FAULT;
            switch (funct3) {
            case FUNCT3_SB:
                if ((addr & 3u) == 0) mem0[index] = x[rs2];
                else if ((addr & 3u) == 1) mem1[index] = x[rs2];
                else if ((addr & 3u) == 2) mem2[index] = x[rs2];
                else mem3[index] = x[rs2];
                break;
            case FUNCT3_SH:
                if ((addr & 1u) != 0) return RV32I_TRAP_LOAD_STORE_ADDRESS_MISALIGNED;
                if ((addr & 2u) == 0) { mem0[index] = x[rs2]; mem1[index] = x[rs2] >> 8; }
                else { mem2[index] = x[rs2]; mem3[index] = x[rs2] >> 8; }
                break;
            case FUNCT3_SW:
                if ((addr & 3u) != 0) return RV32I_TRAP_LOAD_STORE_ADDRESS_MISALIGNED;
                mem0[index] = x[rs2]; mem1[index] = x[rs2] >> 8;
                mem2[index] = x[rs2] >> 16; mem3[index] = x[rs2] >> 24;
                break;
            default: return RV32I_STOP_INVALID_INSTRUCTION;
            }
            pc += 4;
            break;
        case OPCODE_OPI:
            switch (funct3) {
            case FUNCT3_ADDI: value = x[rs1] + simm_i; break;
            case FUNCT3_SLTI: value = (int32_t)x[rs1] < (int32_t)simm_i; break;
            case FUNCT3_SLTIU: value = x[rs1] < simm_i; break;
            case FUNCT3_XORI: value = x[rs1] ^ simm_i; break;
            case FUNCT3_ORI: value = x[rs1] | simm_i; break;
            case FUNCT3_ANDI: value = x[rs1] & simm_i; break;
            case FUNCT3_SLLI:
                if (funct7 != FUNCT7_SLLI) return RV32I_STOP_INVALID_INSTRUCTION;
                value = x[rs1] << ((instr >> 20) & 31u); break;
            case FUNCT3_SRLI:
                if (funct7 == FUNCT7_SRLI) value = x[rs1] >> ((instr >> 20) & 31u);
                else if (funct7 == FUNCT7_SRAI) value = (uint32_t)((int32_t)x[rs1] >> ((instr >> 20) & 31u));
                else return RV32I_STOP_INVALID_INSTRUCTION;
                break;
            default: return RV32I_STOP_INVALID_INSTRUCTION;
            }
            if (rd != 0) x[rd] = value;
            pc += 4;
            break;
        case OPCODE_OP:
            switch (funct3) {
            case FUNCT3_ADD:
                if (funct7 == FUNCT7_ADD) value = x[rs1] + x[rs2];
                else if (funct7 == FUNCT7_SUB) value = x[rs1] - x[rs2];
                else return RV32I_STOP_INVALID_INSTRUCTION;
                break;
            case FUNCT3_SLL: if (funct7 != FUNCT7_SLL) return RV32I_STOP_INVALID_INSTRUCTION; value = x[rs1] << (x[rs2] & 31u); break;
            case FUNCT3_SLT: if (funct7 != FUNCT7_SLT) return RV32I_STOP_INVALID_INSTRUCTION; value = (int32_t)x[rs1] < (int32_t)x[rs2]; break;
            case FUNCT3_SLTU: if (funct7 != FUNCT7_SLTU) return RV32I_STOP_INVALID_INSTRUCTION; value = x[rs1] < x[rs2]; break;
            case FUNCT3_XOR: if (funct7 != FUNCT7_XOR) return RV32I_STOP_INVALID_INSTRUCTION; value = x[rs1] ^ x[rs2]; break;
            case FUNCT3_SRL:
                if (funct7 == FUNCT7_SRL) value = x[rs1] >> (x[rs2] & 31u);
                else if (funct7 == FUNCT7_SRA) value = (uint32_t)((int32_t)x[rs1] >> (x[rs2] & 31u));
                else return RV32I_STOP_INVALID_INSTRUCTION;
                break;
            case FUNCT3_OR: if (funct7 != FUNCT7_OR) return RV32I_STOP_INVALID_INSTRUCTION; value = x[rs1] | x[rs2]; break;
            case FUNCT3_AND: if (funct7 != FUNCT7_AND) return RV32I_STOP_INVALID_INSTRUCTION; value = x[rs1] & x[rs2]; break;
            default: return RV32I_STOP_INVALID_INSTRUCTION;
            }
            if (rd != 0) x[rd] = value;
            pc += 4;
            break;
        case OPCODE_FENCE:
            if (funct3 != 0 || rd != 0 || rs1 != 0 || ((instr >> 28) & 0xfu) != 0)
                return RV32I_STOP_INVALID_INSTRUCTION;
            pc += 4;
            break;
        case OPCODE_ECALL:
            if (instr == 0x00000073u) return RV32I_STOP_ECALL;
            if (instr == 0x00100073u) return RV32I_STOP_EBREAK;
            return RV32I_STOP_INVALID_INSTRUCTION;
        default:
            return RV32I_STOP_INVALID_INSTRUCTION;
        }
    }
}
