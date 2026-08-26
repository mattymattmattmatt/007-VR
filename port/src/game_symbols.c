/*
 * game_symbols.c - the last handful of symbols the port has to supply itself.
 *
 * Two groups, and they are here for opposite reasons.
 *
 * The microcode blobs are RSP programs, assembled into the ROM build from
 * .s files. This port never runs them: libultra.c intercepts the SP task
 * before it would reach the RSP and hands the display list to the renderer
 * instead, which is the whole reason GoldenEye's custom microcode is not a
 * problem here. rsp.c still fills in a task structure that points at them and
 * measures their length, so they have to *exist* -- but nothing ever executes
 * a byte, so what they contain does not matter. They are sized rather than
 * empty so the lengths rsp.c computes stay positive and sane.
 *
 * The crash-handler internals are the opposite: real functions in the SDK's
 * thread queue, in files the port replaces wholesale. src/crash.c walks the
 * faulted thread to print a register dump. The port's threads are pthreads
 * with no such queue, so these keep crash.c linking and are honest about
 * having nothing to say.
 */
#include "platform.h"

#include <ultra64.h>

/* ------------------------------------------------------------ microcode */

/*
 * Aligned to 8 because the task structure wants u64 pointers. The sizes are
 * arbitrary; they only feed ucode_boot_size and friends, which the port's
 * task interception reads past.
 */
long long int rspbootTextStart[64];
long long int rspbootTextEnd[1];

long long int gsp3DTextStart[64];
long long int gsp3DDataStart[64];

long long int aspMainTextStart[64];
long long int aspMainDataStart[64];

/* --------------------------------------------------- crash-handler stubs */

/*
 * __osRunQueue is the SDK's list of runnable threads. The port schedules with
 * pthreads and keeps no such list, so this is an empty one.
 */
OSThread *__osRunQueue;

void __osEnqueueThread(OSThread **queue, OSThread *t)
{
    (void)queue;
    (void)t;
    /* Nothing to enqueue onto: the host scheduler owns runnability here. */
}

/*
 * On the console this returns the thread that took the exception, which
 * crash.c then dumps. A fault on the host arrives as a signal, not as a
 * thread state the game can inspect, so there is nothing truthful to return.
 */
OSThread *__osGetCurrFaultedThread(void)
{
    return NULL;
}
