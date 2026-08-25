/*
 * Tests for the SP task interception.
 *
 * This is the seam where the game's display list stops being the RSP's problem
 * and becomes the renderer's. Getting it wrong is expensive in a way tests
 * catch cheaply: dispatching on both Load and StartGo renders every frame
 * twice, and mixing up the task types feeds audio data to the triangle
 * decoder.
 */
#include "gfxhook.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/os.h>
#include <PR/sptask.h>
#include <PR/mbi.h>
#include <PR/gbi.h>

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

static int         g_gfx_calls;
static const void *g_gfx_dl;
static unsigned    g_gfx_bytes;
static void       *g_gfx_user_seen;

static int         g_aud_calls;
static unsigned    g_aud_bytes;

static void on_gfx(const void *dl, unsigned bytes, void *user)
{
    g_gfx_calls++;
    g_gfx_dl = dl;
    g_gfx_bytes = bytes;
    g_gfx_user_seen = user;
}

static void on_aud(const void *data, unsigned bytes, void *user)
{
    (void)data; (void)user;
    g_aud_calls++;
    g_aud_bytes = bytes;
}

static void reset(void)
{
    g_gfx_calls = 0;
    g_gfx_dl = NULL;
    g_gfx_bytes = 0;
    g_gfx_user_seen = NULL;
    g_aud_calls = 0;
    g_aud_bytes = 0;
    spResetTaskCounts();
}

static Gfx g_dl[16];

static void fill_task(OSTask *t, u32 type, void *data, u32 bytes)
{
    memset(t, 0, sizeof(*t));
    t->t.type = type;
    t->t.data_ptr = (u64 *)data;
    t->t.data_size = bytes;
}

static void test_gfx_routes_once(void)
{
    OSTask task;
    int marker = 0;

    printf("sptask: a graphics task reaches the handler exactly once\n");

    reset();
    spSetGfxTaskHandler(on_gfx, &marker);
    fill_task(&task, M_GFXTASK, g_dl, 8 * (u32)sizeof(Gfx));

    /* The scheduler in src/sched.c calls both in sequence. Dispatching on
     * each would draw every frame twice. */
    osSpTaskLoad(&task);
    osSpTaskStartGo(&task);

    CHECK(g_gfx_calls == 1);
    CHECK(g_gfx_dl == g_dl);
    CHECK(g_gfx_bytes == 8 * sizeof(Gfx));
    CHECK(g_gfx_user_seen == &marker);
    CHECK(spGfxTaskCount() == 1);
}

static void test_byte_count_is_exact(void)
{
    OSTask task;

    printf("sptask: the byte count bounds the list exactly\n");

    reset();
    spSetGfxTaskHandler(on_gfx, NULL);

    /* rspGfxTaskStart computes data_size as (gdl - firstGdl) * sizeof(Gfx),
     * so it is a whole number of commands and the renderer can divide safely. */
    fill_task(&task, M_GFXTASK, g_dl, 3 * (u32)sizeof(Gfx));
    osSpTaskLoad(&task);
    CHECK(g_gfx_bytes / sizeof(Gfx) == 3);

    fill_task(&task, M_GFXTASK, g_dl, 0);
    osSpTaskLoad(&task);
    /* A zero-length list still reaches the handler; deciding to ignore it is
     * the renderer's call, not this layer's. */
    CHECK(g_gfx_calls == 2);
    CHECK(g_gfx_bytes == 0);
}

static void test_task_types_are_separated(void)
{
    OSTask task;

    printf("sptask: audio and graphics go to different handlers\n");

    reset();
    spSetGfxTaskHandler(on_gfx, NULL);
    spSetAudTaskHandler(on_aud, NULL);

    fill_task(&task, M_AUDTASK, g_dl, 64);
    osSpTaskLoad(&task);

    CHECK(g_aud_calls == 1);
    CHECK(g_aud_bytes == 64);
    /* Feeding audio data to the triangle decoder would be spectacular. */
    CHECK(g_gfx_calls == 0);
    CHECK(spAudTaskCount() == 1);
    CHECK(spGfxTaskCount() == 0);

    /* Task types the SDK defines but GoldenEye never submits are ignored
     * rather than misrouted. */
    fill_task(&task, M_VIDTASK, g_dl, 64);
    osSpTaskLoad(&task);
    CHECK(g_gfx_calls == 0);
    CHECK(g_aud_calls == 1);
}

static void test_no_handler_is_safe(void)
{
    OSTask task;

    printf("sptask: tasks before a handler is registered are counted, not crashed on\n");

    reset();
    spSetGfxTaskHandler(NULL, NULL);
    spSetAudTaskHandler(NULL, NULL);

    fill_task(&task, M_GFXTASK, g_dl, 64);
    osSpTaskLoad(&task);
    osSpTaskStartGo(&task);

    /* The game submits frames during boot before video is up. */
    CHECK(spGfxTaskCount() == 1);
    CHECK(g_gfx_calls == 0);

    /* A null task must not dereference. */
    osSpTaskLoad(NULL);
    osSpTaskStartGo(NULL);
    CHECK(spGfxTaskCount() == 1);
}

static void test_yield_reports_done(void)
{
    OSTask task;

    printf("sptask: nothing yields, because tasks complete synchronously\n");

    fill_task(&task, M_GFXTASK, g_dl, 64);
    osSpTaskYield();
    /* The list has already been fully consumed inside osSpTaskLoad, so the
     * game must never be told to resume a partial task. */
    CHECK(osSpTaskYielded(&task) == 0);
}

int main(void)
{
    platformInit();

    printf("ge007 port: SP task interception tests\n\n");

    test_gfx_routes_once();
    test_byte_count_is_exact();
    test_task_types_are_separated();
    test_no_handler_is_safe();
    test_yield_reports_done();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    platformShutdown();
    return g_failures ? 1 : 0;
}
