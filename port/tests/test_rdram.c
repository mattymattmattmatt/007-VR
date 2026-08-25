/*
 * Tests for the RDRAM arena.
 *
 * The property under test is the one the whole 64-bit decision rests on: every
 * pointer the game can see must survive a round trip through
 * osVirtualToPhysical, which truncates to 32 bits by declaration.
 */
#include "platform.h"
#include "rdram.h"

#include <stdio.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/os.h>

static int g_failures;
static int g_checks;

static void check(int cond, const char *what, const char *file, int line)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("  FAIL %s:%d: %s\n", file, line, what);
    }
}

#define CHECK(c) check((c), #c, __FILE__, __LINE__)

static void test_arena_is_low(void)
{
    unsigned long long base, end;

    printf("rdram: the arena lands below 4 GB\n");

    CHECK(rdramInit() == 0);
    CHECK(rdramIsReady());
    CHECK(rdramBase() != NULL);
    CHECK(rdramSize() == GEPC_RDRAM_SIZE);

    base = (unsigned long long)(size_t)rdramBase();
    end = base + rdramSize();

    /* The whole point. If this fails the port must not run. */
    CHECK(end <= 0x100000000ULL);
    CHECK(base > 0);

    /* Re-initialising is a no-op rather than a leak or a second arena. */
    {
        void *first = rdramBase();
        CHECK(rdramInit() == 0);
        CHECK(rdramBase() == first);
    }
}

static void test_round_trip(void)
{
    unsigned char *base = (unsigned char *)rdramBase();
    void *probes[5];
    int i;

    printf("rdram: game pointers survive osVirtualToPhysical\n");

    probes[0] = base;
    probes[1] = base + 1;
    probes[2] = base + (rdramSize() / 2);
    probes[3] = base + rdramSize() - 8;
    probes[4] = base + 0x1234;

    for (i = 0; i < 5; i++) {
        u32 phys = osVirtualToPhysical(probes[i]);
        void *back = osPhysicalToVirtual(phys);
        CHECK(back == probes[i]);
    }
}

static void test_contains(void)
{
    unsigned char *base = (unsigned char *)rdramBase();
    int stack_local = 0;

    printf("rdram: containment is exact at both ends\n");

    CHECK(rdramContains(base));
    CHECK(rdramContains(base + rdramSize() - 1));

    /* One past the end is outside, not inside. */
    CHECK(!rdramContains(base + rdramSize()));
    CHECK(!rdramContains(base - 1));
    CHECK(!rdramContains(NULL));

    /* A stack address is port memory, not game memory. */
    CHECK(!rdramContains(&stack_local));
}

static void test_alloc(void)
{
    void *a, *b, *c;

    printf("rdram: bump allocation aligns and bounds\n");

    a = rdramAlloc(16, 8);
    CHECK(a != NULL);
    CHECK(rdramContains(a));
    CHECK(((size_t)a & 7u) == 0);

    b = rdramAlloc(1, 64);
    CHECK(b != NULL);
    CHECK(((size_t)b & 63u) == 0);
    CHECK(b != a);

    /* Allocations do not overlap. */
    CHECK((unsigned char *)b >= (unsigned char *)a + 16);

    /* A non-power-of-two alignment is rounded up rather than producing a
     * broken mask. */
    c = rdramAlloc(8, 24);
    CHECK(c != NULL);
    CHECK(((size_t)c & 31u) == 0);

    /* Zero size is refused rather than returning a colliding pointer. */
    CHECK(rdramAlloc(0, 8) == NULL);

    /* Asking for more than the arena holds fails cleanly. */
    CHECK(rdramAlloc(GEPC_RDRAM_SIZE * 2, 8) == NULL);
    /* ...and leaves the arena usable. */
    CHECK(rdramAlloc(16, 8) != NULL);
}

static void test_pool_span(void)
{
    unsigned char *start;
    unsigned size;
    unsigned char *base = (unsigned char *)rdramBase();

    printf("rdram: the leftover span handed to the game's pool allocator\n");

    start = (unsigned char *)rdramPoolStart();
    size = rdramPoolSize();

    CHECK(start != NULL);
    CHECK(size > 0);
    CHECK(((size_t)start & 15u) == 0);

    /* The pool is the tail of the arena and nothing more. */
    CHECK(start >= base);
    CHECK(start + size == base + rdramSize());
    CHECK(rdramContains(start));
    CHECK(rdramContains(start + size - 1));

    /* The far end of the pool still round-trips, which is what matters when
     * the game allocates a display list near the top of memory. */
    {
        void *high = start + size - 16;
        CHECK(osPhysicalToVirtual(osVirtualToPhysical(high)) == high);
    }
}

static void test_zeroed(void)
{
    const unsigned char *base = (const unsigned char *)rdramBase();
    unsigned i;
    int nonzero = 0;

    printf("rdram: the arena comes up zeroed\n");

    /* Hardware powers on zeroed and the game's allocator assumes it. Sample
     * the tail, which the port's own bump allocations have not touched. */
    for (i = rdramSize() - 4096; i < rdramSize(); i++) {
        if (base[i] != 0) {
            nonzero++;
        }
    }
    CHECK(nonzero == 0);
}

int main(void)
{
    platformInit();

    printf("ge007 port: RDRAM arena tests\n\n");

    test_arena_is_low();
    if (!rdramIsReady()) {
        printf("\nRDRAM unavailable; remaining tests skipped\n");
        return 1;
    }
    test_round_trip();
    test_contains();
    test_alloc();
    test_pool_span();
    test_zeroed();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    rdramShutdown();
    platformShutdown();
    return g_failures ? 1 : 0;
}
