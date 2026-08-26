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

/*
 * The arena is at a fixed address, so the only way to lose it is for something
 * else to be there first -- and something else was. The kernel randomises the
 * start of the brk heap to somewhere above the executable's data segment, over
 * a range that covers 0x800000-0x1000000, and a few runs in a thousand it
 * landed inside the arena. mmap(MAP_FIXED_NOREPLACE) then correctly refused
 * and the game would not start, with nothing to separate that run from the
 * many before it.
 *
 * Re-running the old test until it passes proves nothing about a one-percent
 * failure, and neither does checking that the heap missed the arena on this
 * particular run -- in a build that got as far as this test, it did, or
 * rdramInit would have failed.
 *
 * What is checked instead is the structure that makes the collision
 * impossible. The kernel derives the initial brk from the end of the last
 * PT_LOAD segment, so once the arena is reserved inside the executable's image
 * the heap is placed *above* it, every time, before main() runs. That is a
 * property of the layout rather than of the run, and one look at
 * /proc/self/maps settles it.
 */
static void test_heap_cannot_reach_the_arena(void)
{
    unsigned long long base = (unsigned long long)(size_t)rdramBase();
    unsigned long long end = base + rdramSize();
    unsigned long long heap_start = 0, heap_end = 0;
    FILE *f;
    char line[512];
    int straddles = 0;

    printf("rdram: the heap is placed where it cannot reach the arena\n");

    f = fopen("/proc/self/maps", "r");
    if (!f) {
        printf("  (no /proc/self/maps; skipped)\n");
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        unsigned long long a, b;

        if (sscanf(line, "%llx-%llx", &a, &b) != 2) {
            continue;
        }
        if (strstr(line, "[heap]")) {
            heap_start = a;
            heap_end = b;
        }
        /* A mapping that crosses the arena's boundary cannot be the arena, so
         * it is something that got in the way. */
        if (b > base && a < end && (a < base || b > end)) {
            straddles++;
            printf("  straddles the arena: %s", line);
        }
    }
    fclose(f);

    CHECK(straddles == 0);

    /* The heap has to exist for any of this to mean anything: the test has
     * malloc'd by now, so an absent [heap] would mean glibc served it from
     * mmap and the interesting case went untested. */
    CHECK(heap_start != 0);
    CHECK(heap_end > heap_start);

#if defined(GEPC_RDRAM_LINKED)
    /*
     * The load-bearing assertion, and the one that separates a fixed build
     * from a lucky one: the heap begins at or after the end of the arena.
     * Without the link-time reservation the heap sits *below* 0x800000 on a
     * good run and inside the arena on a bad one, so this fails there -- which
     * is the point. Configure with -DGEPC_RDRAM_LINK_ARENA=OFF to see it.
     */
    CHECK(heap_start >= end);
#else
    /* Without the reservation nothing places the heap for us: getting this far
     * only means it happened to miss this time. Say where it landed rather
     * than reporting a pass that carries no guarantee. */
    printf("  note: no link-time reservation, so this only means the heap "
           "missed. It is at 0x%llx, the arena at 0x%llx-0x%llx.\n",
           heap_start, base, end);
#endif
}

/*
 * The address is not a preference, it is the contract. boss.c finds the game's
 * memory pool by taking the address of _bssSegmentEnd, which --defsym fixes at
 * 0x800000 when the executable is linked. An arena anywhere else hands the
 * pool memory nothing has mapped.
 */
static void test_arena_is_at_the_linked_address(void)
{
    unsigned long long base = (unsigned long long)(size_t)rdramBase();

    printf("rdram: the arena is where the linker was told to put it\n");

    CHECK(base == 0x800000ULL);

    /* Both ends carry segment index 0, or a direct pointer near the top of
     * the arena reads as a segmented address and resolves to nothing. */
    CHECK(((base >> 24) & 0x0FULL) == 0ULL);
    CHECK((((base + rdramSize() - 1) >> 24) & 0x0FULL) == 0ULL);

#if defined(GEPC_RDRAM_LINKED)
    /* Reserved in the image: writing to both ends must not fault, and the
     * section must really be the size rdram.h claims. */
    {
        extern unsigned char gepcRdramArena[];

        CHECK((void *)gepcRdramArena == rdramBase());
        gepcRdramArena[0] = 0x5A;
        gepcRdramArena[GEPC_RDRAM_SIZE - 1] = 0xA5;
        CHECK(gepcRdramArena[0] == 0x5A);
        CHECK(gepcRdramArena[GEPC_RDRAM_SIZE - 1] == 0xA5);
        gepcRdramArena[0] = 0;
        gepcRdramArena[GEPC_RDRAM_SIZE - 1] = 0;
    }
#endif
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
    test_arena_is_at_the_linked_address();
    test_heap_cannot_reach_the_arena();
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
