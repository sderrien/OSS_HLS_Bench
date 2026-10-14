#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RV32I_GCD_Z8 0, 0, 0, 0, 0, 0, 0, 0
#define RV32I_GCD_Z16 RV32I_GCD_Z8, RV32I_GCD_Z8
#define RV32I_GCD_Z32 RV32I_GCD_Z16, RV32I_GCD_Z16
#define RV32I_GCD_Z64 RV32I_GCD_Z32, RV32I_GCD_Z32

/*
 * Role:
 * Embed a hand-assembled RV32I_MINI program that computes gcd(21, 15).
 *
 * Goal:
 * Exercise only addi, add, lw, sw, and blt. The all-zero word after the program
 * is intentionally unsupported and stops the interpreter after storing the GCD
 * result at byte address 520 and a pass marker at byte address 524.
 */
uint32_t gcd_words[132] = {
    0x20002283, 0x20402303, 0x00100393, 0x0062cc63,
    0x02534663, 0x20502423, 0x00100493, 0x20902623,
    0x00000000, 0x00028413, 0x00804463, 0xfe7040e3,
    0xfff30313, 0xfff40413, 0xfe7048e3, 0x00030413,
    0x00804463, 0xfc7044e3, 0xfff28293, 0xfff40413,
    0xfe7048e3,
    RV32I_GCD_Z64,
    RV32I_GCD_Z32,
    RV32I_GCD_Z8,
    0, 0, 0,
    21, 15, 0, 0,
};

unsigned char *gcd_bin = (unsigned char *)gcd_words;
unsigned int gcd_bin_len = sizeof(gcd_words);

#undef RV32I_GCD_Z8
#undef RV32I_GCD_Z16
#undef RV32I_GCD_Z32
#undef RV32I_GCD_Z64

#ifdef __cplusplus
}
#endif
