#include "audio.h"

#include "platform.h"

#include <pthread.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/os.h>

/* A second of 22 kHz stereo is a little under 90 KB. Double that gives room
 * for the game to run ahead without the ring ever being the constraint. */
#define RING_BYTES (256u * 1024u)

static unsigned char   g_ring[RING_BYTES];
static unsigned        g_head;      /* write position */
static unsigned        g_tail;      /* read position  */
static unsigned        g_queued;
static unsigned        g_rate;
static unsigned        g_underruns;
static int             g_ready;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int audioInit(void)
{
    pthread_mutex_lock(&g_lock);
    g_head = g_tail = g_queued = 0;
    g_underruns = 0;
    g_ready = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void audioShutdown(void)
{
    pthread_mutex_lock(&g_lock);
    g_ready = 0;
    g_head = g_tail = g_queued = 0;
    pthread_mutex_unlock(&g_lock);
}

int audioIsReady(void)      { return g_ready; }
unsigned audioGetRate(void) { return g_rate; }

unsigned audioQueuedBytes(void)
{
    unsigned n;
    pthread_mutex_lock(&g_lock);
    n = g_queued;
    pthread_mutex_unlock(&g_lock);
    return n;
}

unsigned audioUnderrunCount(void)
{
    return g_underruns;
}

void audioResetCounters(void)
{
    g_underruns = 0;
}

void audioFlush(void)
{
    pthread_mutex_lock(&g_lock);
    g_head = g_tail = g_queued = 0;
    pthread_mutex_unlock(&g_lock);
}

static void push_locked(const unsigned char *src, unsigned bytes)
{
    unsigned space = RING_BYTES - g_queued;
    unsigned first;

    if (bytes > space) {
        /* Dropping the oldest audio rather than the newest keeps latency
         * bounded; the alternative is a growing delay between the action on
         * screen and the sound of it. */
        unsigned drop = bytes - space;
        if (drop > g_queued) {
            drop = g_queued;
        }
        g_tail = (g_tail + drop) % RING_BYTES;
        g_queued -= drop;

        if (bytes > RING_BYTES) {
            /* A single write larger than the whole ring: keep only its tail. */
            src += bytes - RING_BYTES;
            bytes = RING_BYTES;
        }
    }

    first = RING_BYTES - g_head;
    if (first > bytes) {
        first = bytes;
    }
    memcpy(g_ring + g_head, src, first);
    if (bytes > first) {
        memcpy(g_ring, src + first, bytes - first);
    }
    g_head = (g_head + bytes) % RING_BYTES;
    g_queued += bytes;
}

unsigned audioPullBytes(void *dst, unsigned bytes)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned avail, first;

    if (!d || !bytes) {
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    avail = (g_queued < bytes) ? g_queued : bytes;

    if (avail < bytes) {
        g_underruns++;
    }

    first = RING_BYTES - g_tail;
    if (first > avail) {
        first = avail;
    }
    memcpy(d, g_ring + g_tail, first);
    if (avail > first) {
        memcpy(d + first, g_ring, avail - first);
    }
    g_tail = (g_tail + avail) % RING_BYTES;
    g_queued -= avail;
    pthread_mutex_unlock(&g_lock);

    return avail;
}

/* ------------------------------------------------------- libultra AI ---- */

s32 osAiSetFrequency(u32 frequency)
{
    /* Hardware derives the actual rate from a divider off the video clock, so
     * the value returned is not necessarily what was asked for. Reporting the
     * request back is right here: the device is opened at this rate, and the
     * game only uses the answer to size its frames. */
    if (frequency < 8000u)  { frequency = 8000u; }
    if (frequency > 48000u) { frequency = 48000u; }

    g_rate = frequency;
    return (s32)frequency;
}

s32 osAiSetNextBuffer(void *bufPtr, u32 size)
{
    if (!bufPtr || !size) {
        return -1;
    }
    if (!g_ready) {
        /* The game queues audio during boot before the device exists.
         * Accepting and dropping it is correct; refusing would make the
         * caller retry forever. */
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    push_locked((const unsigned char *)bufPtr, size);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

u32 osAiGetLength(void)
{
    /* src/audi.c computes its next frame size as
     *   g_FrameSize - (osAiGetLength() >> 2) + slack
     * so this must fall as the device consumes audio. A constant zero makes
     * the game generate maximum frames every time and run away from the DAC. */
    return audioQueuedBytes();
}

u32 osAiGetStatus(void)
{
    /* Bit 31 set means "full" on hardware. Reporting full while the ring has
     * no room keeps the game from spinning on a queue that cannot accept. */
    return (audioQueuedBytes() >= RING_BYTES) ? 0x80000001u : 0u;
}
