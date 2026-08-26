/*
 * game_random.c - the game's random number generators, in C.
 *
 * src/random.s, src/game/chrObjRandom.s and src/tlb_random.s are MIPS
 * assembly, which this port does not assemble. All three are the same
 * generator over a separate 64-bit seed, and all three start from the same
 * constant.
 *
 * The sequence matters. It drives guard reaction times, weapon spread,
 * ammunition drops and the idle animations characters pick, so a generator
 * that is merely "random enough" would change how the game plays in ways that
 * are hard to notice and impossible to attribute. So this follows the
 * instructions rather than tidying them into something shorter:
 *
 *     dsll32 a2, s, 31   ; a2 = s << 63      (dsll32 shifts by sa + 32)
 *     dsll   a1, s, 31   ; a1 = s << 31
 *     dsrl   a2, a2, 31  ; a2 = a2 >>u 31
 *     dsrl32 a1, a1, 0   ; a1 = a1 >>u 32
 *     dsll32 a0, s, 12   ; a0 = s << 44
 *     or     a2, a2, a1
 *     dsrl32 a0, a0, 0   ; a0 = a0 >>u 32
 *     xor    a2, a2, a0
 *     dsrl   a0, a2, 20
 *     andi   a0, a0, 0xfff
 *     xor    a0, a0, a2  ; the new seed
 *     dsll32 v0, a0, 0
 *     dsra32 v0, v0, 0   ; return the low 32 bits
 *
 * The shifts are 64-bit and logical, which is what u64 gives here. The final
 * pair sign-extends the low word into the 64-bit return register; the callers
 * all take a u32, so the low word is the answer either way.
 */
#include <PR/ultratypes.h>

/* .word 0xAB8D9F77, 0x81280783 -- big-endian, so one 64-bit constant. */
#define GEPC_RANDOM_INITIAL_SEED 0xAB8D9F7781280783ULL

u64 g_randomSeed       = GEPC_RANDOM_INITIAL_SEED;
u64 g_chrObjRandomSeed = GEPC_RANDOM_INITIAL_SEED;
u64 g_tlbRandomSeed    = GEPC_RANDOM_INITIAL_SEED;

static u32 advance(u64 *seed)
{
    u64 s = *seed;
    u64 a2 = (s << 63) >> 31;
    u64 a1 = (s << 31) >> 32;
    u64 a0 = (s << 44) >> 32;

    a2 = a2 | a1;
    a2 = a2 ^ a0;
    a0 = ((a2 >> 20) & 0xFFFu) ^ a2;

    *seed = a0;
    return (u32)a0;
}

u32 randomGetNext(void)              { return advance(&g_randomSeed); }
u32 randomGetNextFrom(u64 *seed)     { return advance(seed); }
u32 chrObjRandomGetNext(void)        { return advance(&g_chrObjRandomSeed); }
u32 tlbRandomGetNext(void)           { return advance(&g_tlbRandomSeed); }

/*
 * The seed is stored one higher than the caller asked for. That is not a
 * transcription slip -- `daddiu $a0, $a0, 1` sits in the instruction before
 * the store in all three files -- and it matters, because seeding with zero
 * would otherwise leave the generator stuck.
 */
void randomSetSeed(u32 seed)         { g_randomSeed = (u64)seed + 1u; }
void chrObjRandomSetSeed(u32 seed)   { g_chrObjRandomSeed = (u64)seed + 1u; }
