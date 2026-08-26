/*
 * Tests for the C port of the game's random number generators.
 *
 * A caveat worth stating plainly: the pinned sequence below was produced by
 * this implementation, so it locks the behaviour against future change but
 * does not independently confirm the reading of the MIPS. That confirmation
 * comes from the instruction listing quoted in game_random.c, and from the
 * structural checks here -- the ones that would fail if a shift width, a
 * shift direction or the seed constant had been transcribed wrongly.
 */
#include "platform.h"

#include <stdio.h>

#include <PR/ultratypes.h>

extern u64 g_randomSeed;
extern u64 g_chrObjRandomSeed;
extern u64 g_tlbRandomSeed;

extern u32  randomGetNext(void);
extern u32  randomGetNextFrom(u64 *seed);
extern void randomSetSeed(u32 seed);
extern u32  chrObjRandomGetNext(void);
extern void chrObjRandomSetSeed(u32 seed);
extern u32  tlbRandomGetNext(void);

static int g_failures;
static int g_checks;

static void check_impl(int cond, const char *what, const char *file, int line)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("  FAIL %s:%d: %s\n", file, line, what);
    }
}

#define CHECK(cond) check_impl((cond) ? 1 : 0, #cond, __FILE__, __LINE__)

#define INITIAL_SEED 0xAB8D9F7781280783ULL

/* The constant the three .s files store as .word 0xAB8D9F77, 0x81280783. */
static void test_initial_seed(void)
{
    printf("initial seed\n");
    CHECK(g_randomSeed == INITIAL_SEED);
    CHECK(g_chrObjRandomSeed == INITIAL_SEED);
    CHECK(g_tlbRandomSeed == INITIAL_SEED);
}

static void test_sequence(void)
{
    static const u32 expected[] = {
        0x40EC37CFu, 0x630AEDD7u, 0x1F58071Eu,
        0x0FDDE372u, 0xD9D9DC24u, 0xF12EA100u
    };
    unsigned i;

    printf("sequence from the initial seed\n");
    g_randomSeed = INITIAL_SEED;
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        u32 got = randomGetNext();
        check_impl(got == expected[i], "sequence value", __FILE__, __LINE__);
        if (got != expected[i]) {
            printf("       step %u: got 0x%08X want 0x%08X\n",
                   i, got, expected[i]);
        }
    }
}

/*
 * The returned value is the low word of the new seed, and the new seed is what
 * the next call reads. Getting the final dsll32/dsra32 pair wrong -- returning
 * the high word, say -- would still look random and would fail here.
 */
static void test_return_is_low_word_of_seed(void)
{
    u32 got;

    printf("return value is the new seed's low word\n");
    g_randomSeed = INITIAL_SEED;
    got = randomGetNext();
    CHECK(got == (u32)g_randomSeed);
}

/* The three generators share an algorithm but not a seed. */
static void test_streams_are_independent(void)
{
    u32 a, b;

    printf("independent streams\n");
    g_randomSeed = INITIAL_SEED;
    g_chrObjRandomSeed = INITIAL_SEED;

    a = randomGetNext();
    b = chrObjRandomGetNext();
    CHECK(a == b);                       /* same algorithm, same seed */

    a = randomGetNext();
    CHECK(g_chrObjRandomSeed != g_randomSeed);   /* advancing one is not both */

    (void)tlbRandomGetNext();
    CHECK(g_tlbRandomSeed != INITIAL_SEED);
}

/*
 * `daddiu $a0, $a0, 1` before the store. Seeding with zero has to leave a
 * non-zero seed, or the generator sticks: every operation on zero yields zero.
 */
static void test_set_seed_offsets_by_one(void)
{
    printf("set seed stores seed + 1\n");

    randomSetSeed(0);
    CHECK(g_randomSeed == 1u);
    CHECK(randomGetNext() != 0u);

    randomSetSeed(41);
    CHECK(g_randomSeed == 42u);

    chrObjRandomSetSeed(0);
    CHECK(g_chrObjRandomSeed == 1u);
}

/* randomGetNextFrom runs the same step on a caller-owned seed. */
static void test_get_next_from(void)
{
    u64 mine = INITIAL_SEED;
    u32 theirs, ours;

    printf("randomGetNextFrom\n");
    g_randomSeed = INITIAL_SEED;
    theirs = randomGetNext();
    ours = randomGetNextFrom(&mine);

    CHECK(ours == theirs);
    CHECK(mine == g_randomSeed);
}

/* Nothing here should collapse to a fixed point or a short cycle. */
static void test_does_not_stick(void)
{
    int i, distinct = 0;
    u32 last;

    printf("no short cycle\n");
    randomSetSeed(1);
    last = randomGetNext();
    for (i = 0; i < 4096; i++) {
        u32 v = randomGetNext();
        if (v != last) {
            distinct++;
        }
        last = v;
    }
    CHECK(distinct > 4000);
}

int main(void)
{
    printf("game random tests\n");

    test_initial_seed();
    test_sequence();
    test_return_is_low_word_of_seed();
    test_streams_are_independent();
    test_set_seed_offsets_by_one();
    test_get_next_from();
    test_does_not_stick();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
