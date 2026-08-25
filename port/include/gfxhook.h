/*
 * gfxhook.h - where the game's finished display list leaves libultra.
 *
 * The game builds a display list, wraps it in an OSTask, and hands it to the
 * scheduler, which calls osSpTaskLoad/osSpTaskStartGo. On hardware the RSP
 * would then execute it. Here the task is intercepted instead and the list
 * goes to the renderer, so the RSP microcode never runs -- which is what makes
 * GoldenEye's custom microcode irrelevant to the port.
 *
 * Routing through a registered handler rather than calling the renderer
 * directly keeps libultra.c free of SDL and GL, and lets the interception be
 * tested with a mock and no GPU.
 */
#ifndef GEPC_GFXHOOK_H
#define GEPC_GFXHOOK_H

#ifdef __cplusplus
extern "C" {
#endif

/* `bytes` is the task's data_size, so the handler knows exactly how long the
 * list is rather than having to trust it is terminated. */
typedef void (*gfx_task_fn)(const void *dl, unsigned bytes, void *user);
typedef void (*aud_task_fn)(const void *data, unsigned bytes, void *user);

void spSetGfxTaskHandler(gfx_task_fn fn, void *user);
void spSetAudTaskHandler(aud_task_fn fn, void *user);

/* Counts of tasks seen, for diagnostics. */
unsigned spGfxTaskCount(void);
unsigned spAudTaskCount(void);
void     spResetTaskCounts(void);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_GFXHOOK_H */
