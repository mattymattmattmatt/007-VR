/*
 * platform.h - the PC port's own services, independent of libultra.
 *
 * This is layer 2 of the three the VR work depends on: the decomp builds an
 * N64 ROM, this directory makes it run on a PC, and vr/ puts it in a headset.
 * See docs/VR/Architecture.md for how the layers fit together.
 *
 * Build recipe for anything that includes the game's headers on a PC:
 *
 *     -D_LANGUAGE_C -idirafter include -idirafter src
 *
 * Both parts matter. PR/ultratypes.h gates every type behind _LANGUAGE_C, so
 * without the define the headers parse but declare nothing. And the repository
 * ships N64 replacements for seven libc headers (stdarg, stddef, string,
 * stdlib, math, assert, limits); -idirafter puts the repo's include/ *after*
 * the system directories, so libc wins for those names and the repo still wins
 * for the PR headers, ultra64.h and friends. Plain -Iinclude makes stdio.h
 * pull in the N64 stdarg.h and the build collapses.
 */
#ifndef GEPC_PLATFORM_H
#define GEPC_PLATFORM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up timing and the thread bookkeeping. Call before anything else. */
void        platformInit(void);
void        platformShutdown(void);

/* Monotonic nanoseconds since platformInit. */
unsigned long long platformGetTimeNs(void);

void        platformSleepNs(unsigned long long ns);

/* Where the player's ROM and any extracted assets live. Never NULL. */
const char *platformGetDataPath(void);

/* Diagnostics. Routed to stderr for now; a log file is a later concern. */
void        platformLog(const char *fmt, ...);
void        platformPanic(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_PLATFORM_H */
