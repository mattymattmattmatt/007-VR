/*
 * Semantic tests for the libultra shim.
 *
 * These pin the behaviour the game actually depends on: FIFO ordering, jam
 * going to the head, the ring buffer wrapping, non-blocking calls reporting
 * -1 rather than waiting, and blocking calls genuinely waiting for another
 * thread. Getting any of these subtly wrong produces a game that boots and
 * then deadlocks or drops input, which is miserable to debug from a headset.
 */
#include "platform.h"

#include <pthread.h>
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

/* ------------------------------------------------------- message queues */

static void test_queue_fifo(void)
{
    OSMesgQueue mq;
    OSMesg buf[4];
    OSMesg got;

    printf("queue: fifo ordering\n");
    osCreateMesgQueue(&mq, buf, 4);

    CHECK(osSendMesg(&mq, (OSMesg)1, OS_MESG_NOBLOCK) == 0);
    CHECK(osSendMesg(&mq, (OSMesg)2, OS_MESG_NOBLOCK) == 0);
    CHECK(osSendMesg(&mq, (OSMesg)3, OS_MESG_NOBLOCK) == 0);

    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0 && got == (OSMesg)1);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0 && got == (OSMesg)2);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0 && got == (OSMesg)3);

    /* Empty again. */
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == -1);
}

static void test_queue_full_and_empty(void)
{
    OSMesgQueue mq;
    OSMesg buf[2];
    OSMesg got;

    printf("queue: full and empty are reported, not waited on\n");
    osCreateMesgQueue(&mq, buf, 2);

    CHECK(osSendMesg(&mq, (OSMesg)10, OS_MESG_NOBLOCK) == 0);
    CHECK(osSendMesg(&mq, (OSMesg)11, OS_MESG_NOBLOCK) == 0);

    /* Third send into a two-slot queue must fail rather than overwrite. */
    CHECK(osSendMesg(&mq, (OSMesg)12, OS_MESG_NOBLOCK) == -1);

    /* And the queue's contents must be undamaged by the refused send. */
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0 && got == (OSMesg)10);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0 && got == (OSMesg)11);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == -1);
}

static void test_queue_jam(void)
{
    OSMesgQueue mq;
    OSMesg buf[4];
    OSMesg got;

    printf("queue: jam goes to the head\n");
    osCreateMesgQueue(&mq, buf, 4);

    osSendMesg(&mq, (OSMesg)1, OS_MESG_NOBLOCK);
    osSendMesg(&mq, (OSMesg)2, OS_MESG_NOBLOCK);
    CHECK(osJamMesg(&mq, (OSMesg)99, OS_MESG_NOBLOCK) == 0);

    /* The jammed message must come out first, ahead of everything queued. */
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0 && got == (OSMesg)99);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0 && got == (OSMesg)1);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0 && got == (OSMesg)2);

    /* Jam into a full queue must refuse, like send. */
    osCreateMesgQueue(&mq, buf, 2);
    osSendMesg(&mq, (OSMesg)1, OS_MESG_NOBLOCK);
    osSendMesg(&mq, (OSMesg)2, OS_MESG_NOBLOCK);
    CHECK(osJamMesg(&mq, (OSMesg)3, OS_MESG_NOBLOCK) == -1);
}

static void test_queue_wraparound(void)
{
    OSMesgQueue mq;
    OSMesg buf[3];
    OSMesg got;
    int i;

    printf("queue: ring wraps without corrupting order\n");
    osCreateMesgQueue(&mq, buf, 3);

    /* Push and pop far more than the ring holds, so `first` wraps repeatedly.
     * An off-by-one in the modulo shows up here and nowhere else. */
    for (i = 0; i < 50; i++) {
        CHECK(osSendMesg(&mq, (OSMesg)(long)(i + 1), OS_MESG_NOBLOCK) == 0);
        CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0);
        CHECK(got == (OSMesg)(long)(i + 1));
    }

    /* Interleaved, keeping the ring partly full across the wrap. */
    osSendMesg(&mq, (OSMesg)100, OS_MESG_NOBLOCK);
    osSendMesg(&mq, (OSMesg)200, OS_MESG_NOBLOCK);
    for (i = 0; i < 20; i++) {
        CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0);
        CHECK(osSendMesg(&mq, (OSMesg)(long)(i + 300), OS_MESG_NOBLOCK) == 0);
    }
}

static void test_queue_null_dest(void)
{
    OSMesgQueue mq;
    OSMesg buf[2];

    printf("queue: null destination consumes and discards\n");
    osCreateMesgQueue(&mq, buf, 2);
    osSendMesg(&mq, (OSMesg)7, OS_MESG_NOBLOCK);

    /* Callers that only care that something arrived pass NULL. */
    CHECK(osRecvMesg(&mq, NULL, OS_MESG_NOBLOCK) == 0);
    CHECK(osRecvMesg(&mq, NULL, OS_MESG_NOBLOCK) == -1);
}

/* -------------------------------------------------- blocking across threads */

typedef struct {
    OSMesgQueue *mq;
    int          received;
    OSMesg       value;
} recv_ctx;

static void *blocking_receiver(void *param)
{
    recv_ctx *ctx = (recv_ctx *)param;

    /* Blocks until the main thread sends. If the shim's blocking path is
     * broken this either spins or never returns, and the test times out. */
    if (osRecvMesg(ctx->mq, &ctx->value, OS_MESG_BLOCK) == 0) {
        ctx->received = 1;
    }
    return NULL;
}

static void test_blocking_recv(void)
{
    OSMesgQueue mq;
    OSMesg buf[2];
    pthread_t th;
    recv_ctx ctx;

    printf("queue: blocking receive waits for a sender\n");

    osCreateMesgQueue(&mq, buf, 2);
    memset(&ctx, 0, sizeof(ctx));
    ctx.mq = &mq;

    pthread_create(&th, NULL, blocking_receiver, &ctx);

    /* Give the receiver time to actually reach the wait. */
    platformSleepNs(50ULL * 1000000ULL);
    CHECK(ctx.received == 0);

    osSendMesg(&mq, (OSMesg)4242, OS_MESG_BLOCK);
    pthread_join(th, NULL);

    CHECK(ctx.received == 1);
    CHECK(ctx.value == (OSMesg)4242);
}

typedef struct {
    OSMesgQueue *mq;
    int          sent;
} send_ctx;

static void *blocking_sender(void *param)
{
    send_ctx *ctx = (send_ctx *)param;

    /* Queue is full on entry, so this must wait for the main thread to drain
     * one slot before it can complete. */
    if (osSendMesg(ctx->mq, (OSMesg)555, OS_MESG_BLOCK) == 0) {
        ctx->sent = 1;
    }
    return NULL;
}

static void test_blocking_send(void)
{
    OSMesgQueue mq;
    OSMesg buf[1];
    OSMesg got;
    pthread_t th;
    send_ctx ctx;

    printf("queue: blocking send waits for room\n");

    osCreateMesgQueue(&mq, buf, 1);
    osSendMesg(&mq, (OSMesg)1, OS_MESG_NOBLOCK); /* now full */

    memset(&ctx, 0, sizeof(ctx));
    ctx.mq = &mq;
    pthread_create(&th, NULL, blocking_sender, &ctx);

    platformSleepNs(50ULL * 1000000ULL);
    CHECK(ctx.sent == 0);

    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0);
    pthread_join(th, NULL);
    CHECK(ctx.sent == 1);

    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0 && got == (OSMesg)555);
}

/* --------------------------------------------------------------- threads */

static volatile int g_thread_ran;
static OSId         g_seen_id;

static void thread_entry(void *arg)
{
    g_thread_ran = (int)(long)arg;
    g_seen_id = osGetThreadId(NULL);
}

static void test_threads(void)
{
    static OSThread t;
    static u8 stack[0x2000];
    int spins;

    printf("threads: create, start, run\n");

    g_thread_ran = 0;
    osCreateThread(&t, 7, thread_entry, (void *)(long)1234,
                   stack + sizeof(stack), 10);

    /* Created but not started must not run. */
    platformSleepNs(20ULL * 1000000ULL);
    CHECK(g_thread_ran == 0);
    CHECK(t.state == OS_STATE_STOPPED);

    osStartThread(&t);

    for (spins = 0; spins < 200 && !g_thread_ran; spins++) {
        platformSleepNs(5ULL * 1000000ULL);
    }
    CHECK(g_thread_ran == 1234);

    /* The thread must be able to identify itself while running. */
    CHECK(g_seen_id == 7);

    osDestroyThread(&t);
}

static void test_thread_priority(void)
{
    static OSThread t;
    static u8 stack[0x1000];

    printf("threads: priority is recorded\n");
    osCreateThread(&t, 3, NULL, NULL, stack + sizeof(stack), 42);
    CHECK(osGetThreadPri(&t) == 42);
    osSetThreadPri(&t, 99);
    CHECK(osGetThreadPri(&t) == 99);
}

/* ---------------------------------------------------------------- timers */

static void test_timer_oneshot(void)
{
    OSTimer timer;
    OSMesgQueue mq;
    OSMesg buf[4];
    OSMesg got;
    int spins;

    printf("timers: one shot fires once\n");

    osCreateMesgQueue(&mq, buf, 4);
    memset(&timer, 0, sizeof(timer));

    /* 30 ms, expressed in the count-register ticks the game thinks in. */
    osSetTimer(&timer, (OSTime)OS_NSEC_TO_CYCLES(30000000ULL), 0,
               &mq, (OSMesg)0xABC);

    for (spins = 0; spins < 200; spins++) {
        if (osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0) {
            break;
        }
        platformSleepNs(5ULL * 1000000ULL);
    }
    CHECK(spins < 200);
    CHECK(got == (OSMesg)0xABC);

    /* A one-shot must not re-arm itself. */
    platformSleepNs(100ULL * 1000000ULL);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == -1);

    osStopTimer(&timer);
}

static void test_timer_interval(void)
{
    OSTimer timer;
    OSMesgQueue mq;
    OSMesg buf[16];
    OSMesg got;
    int count = 0;
    int spins;

    printf("timers: interval repeats, and stops when told\n");

    osCreateMesgQueue(&mq, buf, 16);
    memset(&timer, 0, sizeof(timer));

    osSetTimer(&timer, (OSTime)OS_NSEC_TO_CYCLES(10000000ULL),
               (OSTime)OS_NSEC_TO_CYCLES(10000000ULL), &mq, (OSMesg)1);

    for (spins = 0; spins < 200 && count < 3; spins++) {
        while (osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0) {
            count++;
        }
        platformSleepNs(5ULL * 1000000ULL);
    }
    CHECK(count >= 3);

    osStopTimer(&timer);
    while (osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0) {
        /* drain whatever was already queued */
    }
    platformSleepNs(60ULL * 1000000ULL);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == -1);
}

static void test_time(void)
{
    OSTime a, b;

    printf("time: monotonic and advancing\n");

    a = osGetTime();
    platformSleepNs(20ULL * 1000000ULL);
    b = osGetTime();

    CHECK(b > a);

    /* 20 ms of real time should be roughly 20 ms of count ticks. Generous
     * bounds: this is checking the unit conversion, not the host's timer. */
    {
        u64 elapsed_ns = OS_CYCLES_TO_NSEC(b - a);
        CHECK(elapsed_ns > 5000000ULL);
        CHECK(elapsed_ns < 500000000ULL);
    }
}

static void test_hardware_noops(void)
{
    int x = 0;

    printf("hardware: address translation is identity in range\n");

    /* The N64 API returns a u32, so this is only an identity for addresses
     * that actually fit in 32 bits. Within that range it must be exact, since
     * the game feeds these values into display lists. */
    CHECK(osPhysicalToVirtual(0x12345678u) == (void *)(uintptr_t)0x12345678u);
    CHECK(osPhysicalToVirtual(0u) == (void *)(uintptr_t)0u);
    CHECK(osPhysicalToVirtual(0xFFFFFFFFu) == (void *)(uintptr_t)0xFFFFFFFFu);

    /* On a 32-bit build every pointer qualifies, so check the real round trip
     * there. On 64-bit, osVirtualToPhysical deliberately panics rather than
     * truncating, so calling it with a stack address is not a valid test --
     * see the comment on that function for why the port wants a 32-bit
     * target. */
    if (sizeof(void *) == 4) {
        CHECK(osPhysicalToVirtual(osVirtualToPhysical(&x)) == (void *)&x);
    }

    CHECK(osGetMemSize() >= 4 * 1024 * 1024);

    /* Cache maintenance must be safe to call and do nothing observable. */
    osInvalDCache(&x, sizeof(x));
    osWritebackDCache(&x, sizeof(x));
    osWritebackDCacheAll();
    CHECK(x == 0);
}

int main(void)
{
    platformInit();

    printf("ge007 port: libultra shim tests\n\n");

    test_queue_fifo();
    test_queue_full_and_empty();
    test_queue_jam();
    test_queue_wraparound();
    test_queue_null_dest();
    test_blocking_recv();
    test_blocking_send();
    test_threads();
    test_thread_priority();
    test_timer_oneshot();
    test_timer_interval();
    test_time();
    test_hardware_noops();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    platformShutdown();
    return g_failures ? 1 : 0;
}
