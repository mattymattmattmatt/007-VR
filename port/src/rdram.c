#include "rdram.h"

#include "platform.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

static unsigned char *g_base;
static unsigned       g_size;
static unsigned       g_used;

int rdramIsReady(void)     { return g_base != NULL; }
void *rdramBase(void)      { return g_base; }
unsigned rdramSize(void)   { return g_size; }

static int fits_in_u32(const void *p, unsigned size)
{
    unsigned long long start = (unsigned long long)(size_t)p;
    return (start + size) <= 0x100000000ULL;
}

int rdramInit(void)
{
    void *p = NULL;

    if (g_base) {
        return 0;
    }

#if defined(_WIN32)
    /* Walk up from 16 MB looking for a free region below 4 GB. VirtualAlloc
     * honours the base address as a hard request, so a busy address simply
     * fails and the next candidate is tried. */
    {
        uintptr_t candidate;
        for (candidate = 0x01000000u; candidate < 0xF0000000u;
             candidate += 0x01000000u) {
            p = VirtualAlloc((LPVOID)candidate, GEPC_RDRAM_SIZE,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (p) {
                break;
            }
        }
    }
#else
#  if defined(MAP_32BIT)
    /* MAP_32BIT exists precisely for this: it maps in the first 2 GB. */
    p = mmap(NULL, GEPC_RDRAM_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) {
        p = NULL;
    }
#  endif
    if (!p) {
        /* Without MAP_32BIT, ask for a low address by hint and verify. The
         * kernel may ignore the hint, so the result is checked rather than
         * trusted. */
        p = mmap((void *)(size_t)0x20000000u, GEPC_RDRAM_SIZE,
                 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            p = NULL;
        } else if (!fits_in_u32(p, GEPC_RDRAM_SIZE)) {
            munmap(p, GEPC_RDRAM_SIZE);
            p = NULL;
        }
    }
#endif

    if (!p) {
        platformLog("could not reserve %u bytes of RDRAM below 4 GB.",
                    GEPC_RDRAM_SIZE);
        platformLog("the game's pointers must fit in a u32 to survive being "
                    "written into display lists; refusing to continue rather "
                    "than corrupt them.");
        return -1;
    }

    if (!fits_in_u32(p, GEPC_RDRAM_SIZE)) {
        platformLog("RDRAM arena landed at %p, above the 4 GB line.", p);
#if defined(_WIN32)
        VirtualFree(p, 0, MEM_RELEASE);
#else
        munmap(p, GEPC_RDRAM_SIZE);
#endif
        return -1;
    }

    g_base = (unsigned char *)p;
    g_size = GEPC_RDRAM_SIZE;
    g_used = 0;

    /* Hardware comes up zeroed and the game's allocator assumes as much. */
    memset(g_base, 0, g_size);

    platformLog("RDRAM: %u bytes at %p (ends at 0x%08lx)",
                g_size, g_base,
                (unsigned long)((size_t)g_base + g_size));
    return 0;
}

void rdramShutdown(void)
{
    if (!g_base) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(g_base, 0, MEM_RELEASE);
#else
    munmap(g_base, g_size);
#endif
    g_base = NULL;
    g_size = 0;
    g_used = 0;
}

int rdramContains(const void *p)
{
    const unsigned char *c = (const unsigned char *)p;
    return g_base && c >= g_base && c < g_base + g_size;
}

void *rdramAlloc(unsigned size, unsigned align)
{
    unsigned offset;

    if (!g_base || !size) {
        return NULL;
    }
    if (align < 8) {
        align = 8;
    }
    /* Round the alignment up to a power of two so the mask below is valid. */
    {
        unsigned a = 8;
        while (a < align && a < 0x10000000u) {
            a <<= 1;
        }
        align = a;
    }

    offset = (g_used + (align - 1)) & ~(align - 1);
    if (offset > g_size || size > g_size - offset) {
        platformLog("RDRAM exhausted: wanted %u bytes, %u free",
                    size, g_size - g_used);
        return NULL;
    }

    g_used = offset + size;
    return g_base + offset;
}

void *rdramPoolStart(void)
{
    if (!g_base) {
        return NULL;
    }
    /* 16-byte aligned, matching what the game's allocator expects of the
     * span it is handed. */
    return g_base + ((g_used + 15u) & ~15u);
}

unsigned rdramPoolSize(void)
{
    unsigned start;

    if (!g_base) {
        return 0;
    }
    start = (g_used + 15u) & ~15u;
    return (start < g_size) ? (g_size - start) : 0;
}
