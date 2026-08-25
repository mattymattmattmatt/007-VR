/*
 * system.c - clock, sleep, paths and diagnostics for the PC port.
 *
 * Kept free of libultra and of the game's headers so it can be built and
 * tested on its own, and so the header-shadowing rules described in
 * platform.h never apply here.
 */
#include "platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <errno.h>
#  include <unistd.h>
#endif

static unsigned long long g_epoch_ns;
static char               g_data_path[1024];

static unsigned long long now_ns_raw(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&counter);
    return (unsigned long long)((counter.QuadPart * 1000000000ULL)
                                / (unsigned long long)freq.QuadPart);
#else
    struct timespec ts;
    /* Monotonic, so the game's timing cannot be dragged around by NTP or by
     * the player changing their clock mid-mission. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL
         + (unsigned long long)ts.tv_nsec;
#endif
}

void platformInit(void)
{
    const char *env;

    g_epoch_ns = now_ns_raw();

    /* An explicit override wins; otherwise assets are expected next to the
     * executable in "data", matching how the Perfect Dark port does it. */
    env = getenv("GE007_DATA");
    if (env && *env) {
        snprintf(g_data_path, sizeof(g_data_path), "%s", env);
    } else {
        snprintf(g_data_path, sizeof(g_data_path), "data");
    }
}

void platformShutdown(void)
{
}

unsigned long long platformGetTimeNs(void)
{
    unsigned long long now = now_ns_raw();

    /* platformInit may not have run yet in early static construction; treat
     * the first observation as the epoch rather than returning something
     * enormous. */
    if (!g_epoch_ns) {
        g_epoch_ns = now;
    }
    return now - g_epoch_ns;
}

void platformSleepNs(unsigned long long ns)
{
#if defined(_WIN32)
    /* Sleep(0) yields; anything shorter than a millisecond is not worth the
     * timer resolution cost of trying to hit exactly. */
    DWORD ms = (DWORD)(ns / 1000000ULL);
    Sleep(ms ? ms : 1);
#else
    struct timespec req;
    struct timespec rem;

    req.tv_sec = (time_t)(ns / 1000000000ULL);
    req.tv_nsec = (long)(ns % 1000000000ULL);

    /* A signal must not turn a sleep into a busy spin or a short sleep. */
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) {
        req = rem;
    }
#endif
}

const char *platformGetDataPath(void)
{
    if (!g_data_path[0]) {
        platformInit();
    }
    return g_data_path;
}

void platformLog(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("[ge007] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void platformPanic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("[ge007] fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);

    abort();
}
