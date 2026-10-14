/*
 * Role:
 * Exercise the RV32I interpreter's B-type immediate reconstruction without
 * relying on a larger program image or an HLS-generated testbench.
 *
 * Goal:
 * Catch regressions in the scattered immediate bit mapping, especially
 * instruction bit 7, which supplies offset bit 11 for both forward and
 * backward branches.
 *
 * How:
 * Each program executes an always-taken BEQ with only immediate bit 11 set.
 * The correct target contains EBREAK; a missing bit 11 instead self-loops and
 * is detected by the test command's timeout.
 */
#include <cstdio>
#include <cstring>

#include "../src/rv32i.h"

static uint32_t instr_mem[MEM_SIZE / 4];
static uint8_t mem0[MEM_SIZE / 4];
static uint8_t mem1[MEM_SIZE / 4];
static uint8_t mem2[MEM_SIZE / 4];
static uint8_t mem3[MEM_SIZE / 4];

/*
 * Role:
 * Reset the complete fixed-memory interpreter state between programs.
 *
 * Goal:
 * Ensure each branch test observes only its own instruction words and cannot
 * pass because of data left by the preceding direction test.
 */
static void clear_state() {
    std::memset(instr_mem, 0, sizeof(instr_mem));
    std::memset(mem0, 0, sizeof(mem0));
    std::memset(mem1, 0, sizeof(mem1));
    std::memset(mem2, 0, sizeof(mem2));
    std::memset(mem3, 0, sizeof(mem3));
}

/*
 * Role:
 * Run one branch-only program and verify its architectural stop reason.
 *
 * Goal:
 * Report a precise failure that identifies which B-immediate direction failed
 * rather than treating a wrong target as a generic interpreter failure.
 */
static int expect_ebreak(uint32_t start_pc, const char *direction) {
    int result = cpu_run(start_pc, instr_mem, mem0, mem1, mem2, mem3);
    if (result == RV32I_STOP_EBREAK)
        return 0;
    std::fprintf(stderr, "%s B-immediate test returned %d, expected EBREAK (%d)\n",
                 direction, result, RV32I_STOP_EBREAK);
    return 1;
}

/*
 * Role:
 * Verify B-type immediate bit 11 for forward and backward always-taken BEQ.
 *
 * Goal:
 * Cover the two sign-extension cases that the benchmark GCD loop depends on.
 */
int main() {
    constexpr uint32_t kEbreak = 0x00100073u;

    clear_state();
    instr_mem[0] = 0x000000e3u;  // beq x0, x0, +2048
    instr_mem[2048] = kEbreak;
    if (expect_ebreak(0, "forward") != 0)
        return 1;

    clear_state();
    instr_mem[0] = kEbreak;
    instr_mem[2048] = 0x800000e3u;  // beq x0, x0, -2048
    return expect_ebreak(2048, "backward");
}
