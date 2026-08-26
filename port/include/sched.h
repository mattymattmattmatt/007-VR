/*
 * sched.h - route <sched.h> to the game's scheduler types, not glibc's.
 *
 * src/game/rsp.h asks for <sched.h> and means src/sched.h, which defines
 * OSScTask and OSSched. On the N64 that resolved fine. Here the repository's
 * directories sit *after* the system ones (deliberately -- see
 * port/include/platform.h), so the include finds glibc's POSIX scheduler
 * header instead, OSScTask is never declared, and rsp.c fails on a type it
 * appears to have included.
 *
 * port/include comes first on the search path, so this shim intercepts the
 * name and forwards to the real one by a path that cannot be shadowed. Same
 * problem, and same shape of answer, as the strings.h shim next door.
 */
#ifndef GEPC_SCHED_SHIM_H
#define GEPC_SCHED_SHIM_H

#include "../../src/sched.h"

#endif /* GEPC_SCHED_SHIM_H */
