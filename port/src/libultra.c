/*
 * libultra.c - the N64 OS calls the game makes, backed by real OS primitives.
 *
 * Scope note: this file implements the parts of libultra the game genuinely
 * relies on for control flow — threads, message queues and timers — plus the
 * no-op cases (cache maintenance, address translation) that only mean
 * something on real hardware.
 *
 * It deliberately does NOT implement the RSP/RDP task calls (osSpTaskLoad,
 * osSpTaskStartGo, osDpSetNextBuffer) or the VI framebuffer calls. A Fast3D
 * port intercepts the finished display list before the RSP would ever see it,
 * so emulating those would be work in service of nothing. Perfect Dark's port
 * leaves them out for the same reason.
 *
 * Two behavioural differences from real hardware, both deliberate:
 *
 *   Scheduling. The N64 is strictly priority-preemptive: the highest-priority
 *   runnable thread always runs. Here the host scheduler decides, and OSPri is
 *   only advisory. Game code that quietly depended on a lower-priority thread
 *   never running while a higher one was runnable can therefore race. Where
 *   that bites, the fix is an explicit message-queue handshake rather than
 *   trying to rebuild a priority scheduler on top of the host's.
 *
 *   Queue synchronisation. OSMesgQueue has no room for a mutex or condition
 *   variable, and the struct layout has to stay as the game's headers declare
 *   it. So every queue shares one global mutex and one global condition
 *   variable, woken by broadcast. With the handful of threads an N64 title
 *   runs, the contention is irrelevant and the correctness is easy to see.
 */
#include "platform.h"
#include "romdata.h"
#include "rdram.h"
#include "gfxhook.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/os.h>
#include <PR/sptask.h>

/* libultra exports this and the OS_*_TO_CYCLES macros divide by it. On real
 * hardware osInitialize sets it to OS_CLOCK_RATE and then scales it by 3/4,
 * so by the time any game code reads it, it is the count-register rate rather
 * than the CPU clock. The port never runs osInitialize, so it is defined here
 * already carrying that post-init value -- seeding it with OS_CLOCK_RATE
 * instead would make every timer and elapsed-time reading run 33% fast. */
OSTime osClockRate = (OSTime)OS_CPU_COUNTER;

/* --------------------------------------------------------------- threads */

/* The PC thread state lives inside OSThread's register-context region. That
 * region is 400 bytes of MIPS registers that mean nothing here, and reusing it
 * keeps OSThread exactly the size and shape the game's headers declare, with
 * no side table and no allocation. */
typedef struct gepc_thread {
    pthread_t   handle;
    void      (*entry)(void *);
    void       *arg;
    int         started;
    int         detached;
} gepc_thread;

#define THREAD_PC(t) ((gepc_thread *)(void *)&(t)->context)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(gepc_thread) <= sizeof(__OSThreadContext),
               "gepc_thread must fit inside OSThread's context region");
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;

/* Which OSThread the calling host thread is running, for osGetThreadId. */
static pthread_key_t g_self_key;
static pthread_once_t g_self_once = PTHREAD_ONCE_INIT;

static void make_self_key(void)
{
    pthread_key_create(&g_self_key, NULL);
}

static void *thread_trampoline(void *param)
{
    OSThread *t = (OSThread *)param;
    gepc_thread *pc = THREAD_PC(t);

    pthread_once(&g_self_once, make_self_key);
    pthread_setspecific(g_self_key, t);

    pthread_mutex_lock(&g_lock);
    t->state = OS_STATE_RUNNING;
    pthread_mutex_unlock(&g_lock);

    if (pc->entry) {
        pc->entry(pc->arg);
    }

    pthread_mutex_lock(&g_lock);
    t->state = OS_STATE_STOPPED;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg,
                    void *sp, OSPri pri)
{
    gepc_thread *pc;

    if (!t) {
        return;
    }

    /* The game supplies its own stack pointer. Host threads bring their own
     * stacks, so it is accepted and ignored. */
    (void)sp;

    memset(t, 0, sizeof(*t));
    t->id = id;
    t->priority = pri;
    t->state = OS_STATE_STOPPED;
    t->next = NULL;
    t->queue = NULL;

    pc = THREAD_PC(t);
    memset(pc, 0, sizeof(*pc));
    pc->entry = entry;
    pc->arg = arg;
    pc->started = 0;
}

void osStartThread(OSThread *t)
{
    gepc_thread *pc;

    if (!t) {
        return;
    }
    pc = THREAD_PC(t);

    pthread_mutex_lock(&g_lock);
    if (pc->started) {
        /* Restarting a thread that is blocked on a queue is how libultra
         * hands control back; here the host does that for us. */
        t->state = OS_STATE_RUNNABLE;
        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pc->started = 1;
    t->state = OS_STATE_RUNNABLE;
    pthread_mutex_unlock(&g_lock);

    if (pthread_create(&pc->handle, NULL, thread_trampoline, t) != 0) {
        platformPanic("osStartThread: could not create host thread for id %d",
                      (int)t->id);
    }
}

void osStopThread(OSThread *t)
{
    if (!t) {
        return;
    }
    /* Real libultra can suspend an arbitrary thread. There is no portable,
     * safe equivalent, and forcing one invites deadlocks in the middle of a
     * malloc. Threads here stop by returning from their entry point, so this
     * records the intent and leaves the thread to notice. */
    pthread_mutex_lock(&g_lock);
    t->state = OS_STATE_STOPPED;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void osDestroyThread(OSThread *t)
{
    gepc_thread *pc;

    if (!t) {
        return;
    }
    pc = THREAD_PC(t);

    if (pc->started && !pc->detached) {
        pc->detached = 1;
        pthread_detach(pc->handle);
    }
    pthread_mutex_lock(&g_lock);
    t->state = OS_STATE_STOPPED;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

/*
 * Declared here rather than included, because <sched.h> cannot reach glibc's
 * from inside this build: port/include/sched.h deliberately intercepts the
 * name and forwards it to the game's src/sched.h, which is what src/game/rsp.h
 * means when it asks for it. Including <sched.h> here therefore pulls in
 * OSSched and no POSIX at all, and sched_yield stays an implicit declaration
 * -- which compiles, returns int by luck, and would break the moment anything
 * cared.
 *
 * POSIX fixes this signature, so writing it out is safe. It is also the same
 * answer the port already gives elsewhere: where a shim owns a standard
 * header's name, code that wants the real one asks for the symbol directly.
 */
extern int sched_yield(void);

void osYieldThread(void)
{
    sched_yield();
}

OSId osGetThreadId(OSThread *t)
{
    if (t) {
        return t->id;
    }
    pthread_once(&g_self_once, make_self_key);
    t = (OSThread *)pthread_getspecific(g_self_key);
    return t ? t->id : 0;
}

void osSetThreadPri(OSThread *t, OSPri pri)
{
    if (!t) {
        pthread_once(&g_self_once, make_self_key);
        t = (OSThread *)pthread_getspecific(g_self_key);
    }
    if (t) {
        t->priority = pri;
    }
}

OSPri osGetThreadPri(OSThread *t)
{
    if (!t) {
        pthread_once(&g_self_once, make_self_key);
        t = (OSThread *)pthread_getspecific(g_self_key);
    }
    return t ? t->priority : 0;
}

/* -------------------------------------------------------- message queues */

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msgbuf, s32 count)
{
    if (!mq) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgbuf;
    pthread_mutex_unlock(&g_lock);
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    s32 slot;

    if (!mq) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK) {
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
        pthread_cond_wait(&g_cond, &g_lock);
    }

    slot = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[slot] = msg;
    mq->validCount++;

    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    if (!mq) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK) {
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
        pthread_cond_wait(&g_cond, &g_lock);
    }

    /* Jam puts the message at the head, so it is read next. */
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;

    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag)
{
    if (!mq) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) {
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
        pthread_cond_wait(&g_cond, &g_lock);
    }

    /* A null destination is legal and means "consume and discard". */
    if (msg) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* Hardware event plumbing. Nothing on a PC raises these, so the registration
 * is accepted and dropped; the callers treat delivery as best-effort. */
void osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg msg)
{
    (void)e; (void)mq; (void)msg;
}

/* ---------------------------------------------------------------- timers */

/* One host thread services every armed timer. The list is small and only
 * touched under the global lock, so a linear sweep costs nothing and avoids a
 * second synchronisation regime. */
#define GEPC_MAX_TIMERS 32

typedef struct timer_slot {
    OSTimer *timer;
    u64      due_ns;      /* absolute, platform clock */
    u64      interval_ns; /* 0 == one shot */
    int      active;
} timer_slot;

static timer_slot g_timers[GEPC_MAX_TIMERS];
static pthread_t  g_timer_thread;
static int        g_timer_running;

static u64 cycles_to_ns(u64 cycles)
{
    /* OS_CPU_COUNTER is the N64's 46.875 MHz count register. The game thinks
     * in those ticks, so timers arrive in them and have to come back out. */
    return (cycles * 1000000000ULL) / (u64)OS_CPU_COUNTER;
}

static u64 ns_to_cycles(u64 ns)
{
    return (ns * (u64)OS_CPU_COUNTER) / 1000000000ULL;
}

static void *timer_main(void *unused)
{
    (void)unused;

    for (;;) {
        u64 now;
        int i;

        pthread_mutex_lock(&g_lock);
        if (!g_timer_running) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        now = platformGetTimeNs();

        for (i = 0; i < GEPC_MAX_TIMERS; i++) {
            timer_slot *s = &g_timers[i];
            if (!s->active || now < s->due_ns) {
                continue;
            }

            if (s->timer && s->timer->mq) {
                OSMesgQueue *mq = s->timer->mq;
                if (mq->validCount < mq->msgCount) {
                    s32 slot = (mq->first + mq->validCount) % mq->msgCount;
                    mq->msg[slot] = s->timer->msg;
                    mq->validCount++;
                    pthread_cond_broadcast(&g_cond);
                }
                /* A full queue drops the tick rather than blocking the timer
                 * thread, which is what the hardware effectively does too. */
            }

            if (s->interval_ns) {
                s->due_ns += s->interval_ns;
                /* If we slept through several periods, do not spin trying to
                 * deliver them all. */
                if (s->due_ns < now) {
                    s->due_ns = now + s->interval_ns;
                }
            } else {
                s->active = 0;
                s->timer = NULL;
            }
        }
        pthread_mutex_unlock(&g_lock);

        /* 1 ms granularity is finer than a 60 Hz title can observe. */
        platformSleepNs(1000000ULL);
    }
    return NULL;
}

static void ensure_timer_thread(void)
{
    if (g_timer_running) {
        return;
    }
    g_timer_running = 1;
    if (pthread_create(&g_timer_thread, NULL, timer_main, NULL) != 0) {
        g_timer_running = 0;
        platformLog("libultra: no timer thread; osSetTimer will not fire");
    }
}

int osSetTimer(OSTimer *t, OSTime countdown, OSTime interval,
               OSMesgQueue *mq, OSMesg msg)
{
    int i;
    u64 now;

    if (!t) {
        return -1;
    }

    t->mq = mq;
    t->msg = msg;
    t->interval = interval;
    t->value = countdown;

    pthread_mutex_lock(&g_lock);
    ensure_timer_thread();
    now = platformGetTimeNs();

    for (i = 0; i < GEPC_MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].timer == t) {
            break; /* re-arming an existing timer */
        }
    }
    if (i == GEPC_MAX_TIMERS) {
        for (i = 0; i < GEPC_MAX_TIMERS; i++) {
            if (!g_timers[i].active) {
                break;
            }
        }
    }
    if (i == GEPC_MAX_TIMERS) {
        pthread_mutex_unlock(&g_lock);
        platformLog("libultra: out of timer slots");
        return -1;
    }

    g_timers[i].timer = t;
    g_timers[i].interval_ns = cycles_to_ns((u64)interval);
    g_timers[i].due_ns = now + cycles_to_ns((u64)countdown);
    g_timers[i].active = 1;

    pthread_mutex_unlock(&g_lock);
    return 0;
}

int osStopTimer(OSTimer *t)
{
    int i;

    if (!t) {
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < GEPC_MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].timer == t) {
            g_timers[i].active = 0;
            g_timers[i].timer = NULL;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

OSTime osGetTime(void)
{
    return (OSTime)ns_to_cycles(platformGetTimeNs());
}

u32 osGetCount(void)
{
    return (u32)ns_to_cycles(platformGetTimeNs());
}

/* ------------------------------------------------------------- PI / DMA */

/* The game reads every asset through romCopy() in src/ramrom.c, which is
 * osPiStartDma followed by a blocking osRecvMesg. Pointing that at the ROM
 * image loaded by romdata is what makes the whole asset pipeline -- the file
 * table, the segment layout, rz decompression -- work unchanged.
 *
 * The completion message is posted on EVERY path, including failure. romCopy
 * blocks on it unconditionally, so a read that quietly returned without
 * posting would hang the game forever instead of showing a bad texture. */
s32 osPiStartDma(OSIoMesg *mb, s32 pri, s32 direction, u32 devAddr,
                 void *dramAddr, u32 nbytes, OSMesgQueue *mq)
{
    long offset;

    (void)pri;

    if (mb) {
        mb->devAddr = devAddr;
        mb->dramAddr = dramAddr;
        mb->size = nbytes;
    }

    if (direction == OS_READ && dramAddr && nbytes) {
        offset = romdataAddrToOffset(devAddr);
        if (offset < 0) {
            platformLog("PI read from 0x%08x is not cartridge space", devAddr);
            memset(dramAddr, 0, nbytes);
        } else if (romdataRead((unsigned)offset, dramAddr, nbytes) != 0) {
            /* Zero rather than leave the buffer undefined: garbage in a
             * display list is far harder to diagnose than empty geometry. */
            memset(dramAddr, 0, nbytes);
        }
    }
    /* OS_WRITE targets the cartridge, which only meant anything on the
     * development hardware. Accepted and dropped. */

    if (mq) {
        osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

s32 osPiRawStartDma(s32 direction, u32 devAddr, void *dramAddr, u32 nbytes)
{
    return osPiStartDma(NULL, OS_MESG_PRI_NORMAL, direction, devAddr,
                        dramAddr, nbytes, NULL);
}

s32 osPiRawReadIo(u32 devAddr, u32 *data)
{
    long offset = romdataAddrToOffset(devAddr);
    unsigned char b[4];

    if (!data) {
        return -1;
    }
    if (offset < 0 || romdataRead((unsigned)offset, b, 4) != 0) {
        *data = 0;
        return -1;
    }
    /* The cartridge bus is big endian regardless of what the host is. */
    *data = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
    return 0;
}

s32 osPiRawWriteIo(u32 devAddr, u32 data)
{
    (void)devAddr; (void)data;
    return 0;
}

void osCreatePiManager(OSPri pri, OSMesgQueue *mq, OSMesg *msg, s32 count)
{
    /* There is no PI queue to service here: osPiStartDma completes inline.
     * The queue the game hands over still has to be a working queue, because
     * it will post to it. */
    (void)pri;
    if (mq && msg && count > 0) {
        osCreateMesgQueue(mq, msg, count);
    }
}

/* ------------------------------------------------------------- SP tasks */

static gfx_task_fn g_gfx_handler;
static void       *g_gfx_user;
static aud_task_fn g_aud_handler;
static void       *g_aud_user;
static unsigned    g_gfx_tasks;
static unsigned    g_aud_tasks;

void spSetGfxTaskHandler(gfx_task_fn fn, void *user)
{
    g_gfx_handler = fn;
    g_gfx_user = user;
}

void spSetAudTaskHandler(aud_task_fn fn, void *user)
{
    g_aud_handler = fn;
    g_aud_user = user;
}

unsigned spGfxTaskCount(void) { return g_gfx_tasks; }
unsigned spAudTaskCount(void) { return g_aud_tasks; }
void spResetTaskCounts(void)  { g_gfx_tasks = 0; g_aud_tasks = 0; }

/* The RSP never runs here. rspGfxTaskStart in src/game/rsp.c fills in
 * data_ptr with the first Gfx and data_size with the list's length in bytes,
 * so the interception gets an exact bound rather than having to trust the
 * list is terminated.
 *
 * Dispatch happens on Load rather than StartGo because the scheduler calls
 * both in sequence and Load is where the task becomes fully described;
 * handling it twice would render every frame twice. */
static void dispatch_task(OSTask *task)
{
    if (!task) {
        return;
    }

    switch (task->t.type) {
    case M_GFXTASK:
        g_gfx_tasks++;
        if (g_gfx_handler) {
            g_gfx_handler(task->t.data_ptr, task->t.data_size, g_gfx_user);
        }
        break;

    case M_AUDTASK:
        g_aud_tasks++;
        if (g_aud_handler) {
            g_aud_handler(task->t.data_ptr, task->t.data_size, g_aud_user);
        }
        break;

    default:
        /* Video and HVQ tasks exist in the SDK but GoldenEye does not use
         * them; ignoring them is correct rather than merely convenient. */
        break;
    }
}

void osSpTaskLoad(OSTask *task)
{
    dispatch_task(task);
}

void osSpTaskStartGo(OSTask *task)
{
    /* Already dispatched by osSpTaskLoad. */
    (void)task;
}

void osSpTaskYield(void)
{
}

OSYieldResult osSpTaskYielded(OSTask *task)
{
    (void)task;
    /* Nothing here ever yields: the task completes synchronously inside
     * osSpTaskLoad, so by the time anything asks, it is done. */
    return 0;
}

/* ------------------------------------------------- hardware-only no-ops */

/* The R4300 needed these to keep its caches coherent with DMA. A PC has one
 * coherent view of memory, so they are correctly empty rather than merely
 * unimplemented. */
void osInvalDCache(void *v, s32 n)      { (void)v; (void)n; }
void osInvalICache(void *v, s32 n)      { (void)v; (void)n; }
void osWritebackDCache(void *v, s32 n)  { (void)v; (void)n; }
void osWritebackDCacheAll(void)         { }

/* No TLB and no KSEG mapping here, so a pointer is already its own address.
 *
 * The catch is that PR/os.h declares this returning u32, because on the N64
 * every address genuinely was 32 bits. The game leans on that: model.c passes
 * osVirtualToPhysical() results straight into display lists via gSPVertex, and
 * the renderer has to turn them back into pointers later.
 *
 * On a 64-bit host a heap pointer does not fit, and quietly dropping the top
 * 32 bits would produce display lists full of addresses that are wrong in a
 * way nothing detects until geometry renders as garbage. So this refuses
 * rather than truncates.
 *
 * The two real fixes, in order of preference:
 *   - build the port for 32-bit (Perfect Dark's port sets TARGET_ARCH i686),
 *     which makes the whole question disappear; or
 *   - place all game-visible memory in an arena below 4 GB.
 * Note that Fast3D resolves *segmented* addresses through its own segment
 * table, so this path only has to cover direct pointers. */
u32 osVirtualToPhysical(void *v)
{
    uintptr_t a = (uintptr_t)v;

    if (sizeof(uintptr_t) > 4 && (unsigned long long)a > 0xFFFFFFFFULL) {
        /* Everything the game can see is supposed to live in the RDRAM arena,
         * which rdram.c places below 4 GB precisely so this cannot happen. A
         * pointer arriving here from outside it means port-side memory has
         * leaked into a game structure -- worth failing loudly, because the
         * alternative is a display list that renders as garbage with no clue
         * where the bad address came from. */
        platformPanic("osVirtualToPhysical: %p is outside the RDRAM arena and "
                      "will not fit in the 32-bit address this API returns. "
                      "Game-visible allocations must come from rdramAlloc or "
                      "the game's own pool.", v);
    }
    return (u32)a;
}

void *osPhysicalToVirtual(u32 p)        { return (void *)(uintptr_t)p; }

OSIntMask osGetIntMask(void)            { return 0; }
OSIntMask osSetIntMask(OSIntMask m)     { (void)m; return 0; }

u32 osGetMemSize(void)
{
    /* The game asks so it can size its heaps. Reporting the 8 MB Expansion
     * Pak configuration keeps every allocation path on its generous branch. */
    return 8 * 1024 * 1024;
}
