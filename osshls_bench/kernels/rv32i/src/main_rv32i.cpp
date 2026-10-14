#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rv32i.h"
#include "mem_image.h"
#include "gcd_image.h"

/* If built with an embedded memory image, `mem_image.cpp` defines:
 *   unsigned char demo_bin[];
 *   unsigned int demo_bin_len;
 */
// extern unsigned char demo_bin[];
// extern unsigned int demo_bin_len;

struct elf32_header {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf32_phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};

void print_bin(uint32_t x) {
    for (uint32_t i = 31; (i + 1) != 0; --i) {
        if (x & (1 << i))
            printf("1");
        else
            printf("0");
    }
}
uint32_t g_instr_mem[MEM_SIZE / 4] = {0};
uint8_t g_mem0[MEM_SIZE / 4] = {0};
uint8_t g_mem1[MEM_SIZE / 4] = {0};
uint8_t g_mem2[MEM_SIZE / 4] = {0};
uint8_t g_mem3[MEM_SIZE / 4] = {0};

#ifdef RV32I_DISABLE_EXTERN_C
/*
 * Role:
 * Declare the C++-linkage RTL wrapper entry signature used by the Verilator
 * runtime flow.
 *
 * Goal:
 * Allow `main_rv32i.cpp` to call the generated C++ wrapper even when the
 * canonical software/cyclesim API in `rv32i.h` keeps byte-lane memories typed
 * as `uint8_t*`.
 */
int cpu_run(int init_pc, int instr_mem[MEM_SIZE / 4], int mem0[MEM_SIZE / 4],
            int mem1[MEM_SIZE / 4], int mem2[MEM_SIZE / 4],
            int mem3[MEM_SIZE / 4]);
#endif

/*
 * Role:
 * Reset all instruction/data memories to a known clean state.
 *
 * Goal:
 * Ensure repeated CPU runs in one process start from identical RAM contents,
 * which is required when we run a reference trace pass before compiled cycle-sim.
 */
static void clear_memories(void) {
    memset(g_instr_mem, 0, sizeof(g_instr_mem));
    memset(g_mem0, 0, sizeof(g_mem0));
    memset(g_mem1, 0, sizeof(g_mem1));
    memset(g_mem2, 0, sizeof(g_mem2));
    memset(g_mem3, 0, sizeof(g_mem3));
}

/*
 * Role:
 * Load the embedded `demo_bin` program image into instruction and byte-lane
 * memories exactly like the hardware model expects.
 *
 * Goal:
 * Provide deterministic program bytes and aligned lane memories for both
 * reference and compiled cycle-sim executions.
 */
static void load_program_image(const unsigned char *image, unsigned int image_len) {
    /*
     * Role:
     * Copy one flat RV32I program image into both instruction memory and the
     * byte-lane data memories used by the interpreter.
     *
     * Goal:
     * Let preprocessor-selected programs share exactly the same memory layout,
     * so switching between the full demo and mini GCD program only changes the
     * input image, not the simulator memory model.
     */
    clear_memories();
    uint32_t bytes = image_len;
    for (uint32_t i = 0; i < bytes && i < MEM_SIZE; ++i) {
        uint32_t base_addr = i / 4;
        uint32_t addr = (i / 4) * 4; /* byte-address used as index in g_instr_mem */
        uint8_t b = image[i];
        switch (i & 3) {
        case 0:
            g_instr_mem[addr] = (g_instr_mem[addr] & 0xFFFFFF00) | (uint32_t)b;
            g_mem0[base_addr] = b;
            break;
        case 1:
            g_instr_mem[addr] = (g_instr_mem[addr] & 0xFFFF00FF) | ((uint32_t)b << 8);
            g_mem1[base_addr] = b;
            break;
        case 2:
            g_instr_mem[addr] = (g_instr_mem[addr] & 0xFF00FFFF) | ((uint32_t)b << 16);
            g_mem2[base_addr] = b;
            break;
        default:
            g_instr_mem[addr] = (g_instr_mem[addr] & 0x00FFFFFF) | ((uint32_t)b << 24);
            g_mem3[base_addr] = b;
            break;
        }
    }
}

static void load_embedded_program(void) {
    /*
     * Role:
     * Select the RV32I program image compiled into this benchmark binary.
     *
     * Goal:
     * Use `RV32I_PROGRAM_GCD` with `RV32I_MINI` for the reduced-instruction GCD
     * test, while preserving the legacy full-ISA demo under
     * `RV32I_PROGRAM_DEMO` for non-mini decoder experiments.
     */
#ifdef RV32I_PROGRAM_GCD
    load_program_image(gcd_bin, gcd_bin_len);
#else
    load_program_image(demo_bin, demo_bin_len);
#endif
}

static uint32_t read_memory_word(uint32_t byte_addr) {
    /*
     * Role:
     * Reconstruct one little-endian 32-bit data-memory word from the four byte
     * lane arrays used by the RV32I interpreter.
     *
     * Goal:
     * Report the GCD program's memory-mapped result without adding any extra ISA
     * instructions or host-only callbacks to the mini core.
     */
    uint32_t base_addr = byte_addr >> 2;
    return ((uint32_t)g_mem0[base_addr]) |
           (((uint32_t)g_mem1[base_addr]) << 8) |
           (((uint32_t)g_mem2[base_addr]) << 16) |
           (((uint32_t)g_mem3[base_addr]) << 24);
}

int main(int argc, char **argv) {
    /*
     * This testbench optionally accepts an output file path in argv[1].
     * When provided, stdout is redirected to that file so every instruction
     * trace line produced by cpu_run is captured in a deterministic artifact
     * for Makefile diff-based verification.
     */
    if (argc > 1) {
        if (freopen(argv[1], "w", stdout) == nullptr) {
            return 3;
        }
    }

    /* Always use embedded memory image. */
    load_embedded_program();

    /* start at PC = 0 */
#ifdef RV32I_DISABLE_EXTERN_C
    cpu_run(0, reinterpret_cast<int *>(g_instr_mem),
            reinterpret_cast<int *>(g_mem0), reinterpret_cast<int *>(g_mem1),
            reinterpret_cast<int *>(g_mem2), reinterpret_cast<int *>(g_mem3));
#else
    cpu_run(0, g_instr_mem, g_mem0, g_mem1, g_mem2, g_mem3);
#endif

#ifdef RV32I_PROGRAM_GCD
    {
        uint32_t gcd_result = read_memory_word(520);
        uint32_t pass_marker = read_memory_word(524);
        printf("gcd_result=%u expected=3\n", gcd_result);
        printf("gcd_pass=%s\n", (gcd_result == 3 && pass_marker == 1) ? "yes" : "no");
    }
#endif
    return 0;
}
