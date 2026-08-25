/*
 * rdram.h - a synthetic RDRAM for the game to live in.
 *
 * This is how the port answers the 32-bit pointer problem without building
 * 32-bit.
 *
 * PR/os.h declares `u32 osVirtualToPhysical(void *)`, because on the N64 every
 * address really was 32 bits, and the game relies on that: src/game/model.c
 * feeds the result straight into display lists through gSPVertex. On a 64-bit
 * host an arbitrary heap pointer does not fit, and truncating it silently
 * produces display lists full of addresses that are wrong in a way nothing
 * detects until geometry renders as garbage.
 *
 * Perfect Dark's port solves this by building for i686. This port instead
 * confines everything the game can see to a single arena placed below 4 GB,
 * so game pointers fit in a u32 naturally while the port itself, the renderer
 * and the OpenXR loader all stay 64-bit. No multilib, no hunting for a 32-bit
 * VR runtime, and room to render stereo.
 *
 * It also matches what the game already expects. src/boss.c hands its pool
 * allocator the span from the end of BSS to the TLB block -- that is, whatever
 * RDRAM is left over. Here that span is this arena, and mempCheckMemflagTokens
 * carves it up exactly as it does on hardware.
 */
#ifndef GEPC_RDRAM_H
#define GEPC_RDRAM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Expansion Pak configuration. osGetMemSize() reports the same number, and
 * src/game/front.c checks for exactly this. */
#define GEPC_RDRAM_SIZE (8u * 1024u * 1024u)

/* Reserves the arena. Returns 0 on success, -1 if no suitable low region could
 * be obtained -- which is fatal for the port, since the alternative is silent
 * display-list corruption. */
int   rdramInit(void);
void  rdramShutdown(void);

void       *rdramBase(void);
unsigned    rdramSize(void);
int         rdramIsReady(void);

/* True if a pointer lies inside the arena, and therefore survives the trip
 * through osVirtualToPhysical intact. */
int   rdramContains(const void *p);

/* Bump allocation out of the arena, for the port's own setup before the
 * game's pool allocator takes over the remainder. Returns NULL when full.
 * `align` is rounded up to a power of two of at least 8. */
void *rdramAlloc(unsigned size, unsigned align);

/* What the game's pool allocator should be given: everything not yet handed
 * out by rdramAlloc. */
void *rdramPoolStart(void);
unsigned rdramPoolSize(void);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_RDRAM_H */
