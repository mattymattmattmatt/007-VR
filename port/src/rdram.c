#include "rdram.h"

#include "platform.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

/* The arena must sit low enough that every address in it carries segment
 * index 0, the way physical RDRAM does on hardware (0x00000000-0x007fffff).
 *
 * Segmented addresses keep their segment number in bits 24-27, so an arena
 * placed anywhere above 16 MB makes every *direct* pointer in a display list
 * look like a segment reference. The symptom is brutal and silent: G_VTX
 * resolves to nothing, no vertices load, and the triangles still get counted
 * and still issue draw calls -- they are simply all degenerate, so the screen
 * stays black. The self-test found it exactly that way, by reading pixels back
 * rather than trusting the counters. */
/*
 * The arena is pinned rather than placed wherever it fits, because the game's
 * memory pool has to start at a link-time constant: boss.c takes the address
 * of _bssSegmentEnd to find it, and that symbol is defined by --defsym when
 * the executable is linked (see port/tools/gen_segment_defsyms.py). An arena
 * that moved would leave the pool pointing somewhere nothing has mapped.
 *
 * 0x800000 is the only base that satisfies both ends. It has to clear the
 * executable, which -no-pie loads at 0x400000, and the arena's 8 MB has to end
 * by 0x1000000 or addresses in it stop carrying segment index 0 -- so the base
 * can be no higher either. Both constraints meet exactly here.
 */
#define GEPC_RDRAM_BASE     0x00800000u
#define GEPC_RDRAM_MAX_END  0x01000000u   /* 16 MB: keeps segment index 0 */

static unsigned char *g_base;
static unsigned       g_size;
static unsigned       g_used;

int rdramIsReady(void)     { return g_base != NULL; }
void *rdramBase(void)      { return g_base; }
unsigned rdramSize(void)   { return g_size; }

static int placement_is_valid(const void *p)
{
    uintptr_t start = (uintptr_t)p;
    uintptr_t end = start + GEPC_RDRAM_SIZE;

    if (!p || end > GEPC_RDRAM_MAX_END) {
        return 0;
    }
    /* Both ends must land in segment 0, or an address near the top of the
     * arena would still be misread as segmented. */
    return (((start >> 24) & 0x0Fu) == 0u) && ((((end - 1) >> 24) & 0x0Fu) == 0u);
}

static void release(void *p)
{
#if defined(_WIN32)
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, GEPC_RDRAM_SIZE);
#endif
}

int rdramInit(void)
{
    void *p = NULL;
    uintptr_t candidate;

    if (g_base) {
        return 0;
    }

    /* Before reserving anything, check the linkage the arena's whole premise
     * rests on. Static display lists live in the executable, not in here. */
    gepcAssertLowMemory();

    /* One address, requested exactly and never merely hinted: a kernel that
     * quietly relocated the mapping somewhere high would reintroduce the bug
     * above, and anywhere other than GEPC_RDRAM_BASE would put the memory
     * pool where _bssSegmentEnd does not point. */
    for (candidate = GEPC_RDRAM_BASE;
         candidate == GEPC_RDRAM_BASE;
         candidate += GEPC_RDRAM_SIZE) {
#if defined(_WIN32)
        p = VirtualAlloc((LPVOID)candidate, GEPC_RDRAM_SIZE,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p) {
            break;
        }
#else
        {
            int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#  if defined(MAP_FIXED_NOREPLACE)
            /* Fails rather than replacing an existing mapping, which plain
             * MAP_FIXED would happily do. */
            flags |= MAP_FIXED_NOREPLACE;
#  endif
            p = mmap((void *)candidate, GEPC_RDRAM_SIZE,
                     PROT_READ | PROT_WRITE, flags, -1, 0);
            if (p == MAP_FAILED) {
                p = NULL;
                continue;
            }
            if ((uintptr_t)p == candidate) {
                break;
            }
            /* Hint ignored: hand it back and keep looking rather than accept
             * an address that breaks the segment-index invariant. */
            munmap(p, GEPC_RDRAM_SIZE);
            p = NULL;
        }
#endif
    }

    if (!p) {
        platformLog("could not reserve %u bytes of RDRAM below 16 MB.",
                    GEPC_RDRAM_SIZE);
        platformLog("game pointers must fit in a u32 and must carry segment "
                    "index 0, or display lists silently render nothing.");
        return -1;
    }

    if (!placement_is_valid(p)) {
        platformLog("RDRAM arena landed at %p, where addresses would be "
                    "mistaken for segmented ones.", p);
        release(p);
        return -1;
    }

    g_base = (unsigned char *)p;
    g_size = GEPC_RDRAM_SIZE;
    g_used = 0;

    /* Hardware comes up zeroed and the game's allocator assumes as much. */
    memset(g_base, 0, g_size);

    platformLog("RDRAM: %u bytes at %p (segment index %u)",
                g_size, g_base,
                (unsigned)(((uintptr_t)g_base >> 24) & 0x0Fu));
    return 0;
}

void rdramShutdown(void)
{
    if (!g_base) {
        return;
    }
    release(g_base);
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

/* Deliberately non-const and non-static-const so both .data and .rodata get
 * sampled; a linker that split them across the 4 GB line would be caught here
 * rather than in the renderer. */
static unsigned g_lowMemoryProbeData = 1u;
static const unsigned g_lowMemoryProbeRodata = 1u;

void gepcAssertLowMemory(void)
{
    const void *probes[2];
    const char *names[2];
    int i;

    probes[0] = (const void *)&g_lowMemoryProbeData;
    names[0]  = ".data";
    probes[1] = (const void *)&g_lowMemoryProbeRodata;
    names[1]  = ".rodata";

    for (i = 0; i < 2; i++) {
        unsigned long long a = (unsigned long long)(uintptr_t)probes[i];

        if (a > 0xFFFFFFFFULL) {
            platformPanic(
                "ge007: %s is linked at 0x%llx, above 4 GB.\n"
                "Display list command words are 32 bits wide, so every pointer "
                "the game stores in one would be truncated and the geometry "
                "would render as garbage without any error.\n"
                "Link the port with -no-pie (see port/CMakeLists.txt).",
                names[i], a);
        }
    }
}
