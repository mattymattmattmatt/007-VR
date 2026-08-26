/*
 * libultra_stubs.c - the remaining SDK surface the game links against.
 *
 * Found by building every translation unit that compiles and asking the linker
 * what was still missing. The answer split cleanly into four groups, and each
 * is treated differently:
 *
 *   Video interface (osVi*) and RDP (osDp*) are REPLACED, not emulated. The
 *   renderer intercepts the display list long before the RSP or the video
 *   interface would see anything, so these exist only to satisfy the linker
 *   and to keep the game's own bookkeeping happy.
 *
 *   Controller (osCont*) is REAL. joy.c drives it every frame and the VR layer
 *   writes its synthetic pads through the same path, so this has to behave.
 *
 *   EEPROM (osEeprom*) is REAL and file-backed, because it is the player's
 *   save data.
 *
 *   TLB, FPU control and SI internals are hardware-only and are stubbed.
 */
#include "platform.h"
#include "video.h"

#include <stdlib.h>
#include "romdata.h"

#include <stdio.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/os.h>

/* ------------------------------------------------------------- globals */

/* 1 == NTSC. GoldenEye reads this to pick frame timing; PAL builds want 0. */
u32 osTvType = 1;

/* Cartridge base in the PI address map. The game only uses it to form ROM
 * addresses, which romdata resolves. */
u32 osRomBase = 0xB0000000u;

/* The game indexes this by video mode. The entries are never consumed here --
 * the renderer sets its own resolution -- but it must exist and be large
 * enough that any index the game uses stays in bounds. */
OSViMode osViModeTable[64];

/* -------------------------------------------------------- controllers */

#define GEPC_MAX_PADS 4

static OSContPad    g_pads[GEPC_MAX_PADS];
static OSContStatus g_status[GEPC_MAX_PADS];
static int          g_cont_ready;

/* Called by the port (and, in the VR build, ultimately by the mapper) to
 * publish the current controller state. Kept separate from osContGetReadData
 * so the read path stays a plain copy. */
void gepcSetControllerPad(int index, const OSContPad *pad)
{
    if (index >= 0 && index < GEPC_MAX_PADS && pad) {
        g_pads[index] = *pad;
    }
}

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status)
{
    int i;

    (void)mq;

    memset(g_pads, 0, sizeof(g_pads));
    memset(g_status, 0, sizeof(g_status));

    /* Report two controllers present. The Goodhead control style the VR layer
     * targets reads two pads, and a game that believes only one is plugged in
     * will not offer that style at all. */
    for (i = 0; i < 2; i++) {
        g_status[i].type = CONT_TYPE_NORMAL;
        g_status[i].status = 0;
        /* The SDK names this field "errno". Any translation unit that pulls
         * in <errno.h> would macro-expand it and break the struct outright,
         * so nothing here includes it. */
        g_status[i].errno = 0;
    }
    for (i = 2; i < GEPC_MAX_PADS; i++) {
        g_status[i].errno = CONT_NO_RESPONSE_ERROR;
    }

    if (bitpattern) {
        *bitpattern = 0x03;   /* controllers 1 and 2 */
    }
    if (status) {
        memcpy(status, g_status, sizeof(g_status));
    }

    g_cont_ready = 1;
    return 0;
}

s32 osContStartReadData(OSMesgQueue *mq)
{
    /* The read completes immediately; the game blocks on the queue straight
     * afterwards, so a message has to be posted or it waits forever -- the
     * same trap as the PI DMA path. */
    if (mq) {
        osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    }
    return 0;
}

void osContGetReadData(OSContPad *pad)
{
    if (pad) {
        memcpy(pad, g_pads, sizeof(g_pads));
    }
}

s32 osContStartQuery(OSMesgQueue *mq)
{
    if (mq) {
        osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    }
    return 0;
}

void osContGetQuery(OSContStatus *status)
{
    if (status) {
        memcpy(status, g_status, sizeof(g_status));
    }
}

s32 osContSetCh(u8 ch)
{
    (void)ch;
    return 0;
}

/* ------------------------------------------------------------- EEPROM */

/* GoldenEye uses a 16 Kbit EEPROM: 256 blocks of 8 bytes. */
#define GEPC_EEPROM_BLOCKS 256
#define GEPC_EEPROM_BLOCK  8
#define GEPC_EEPROM_BYTES  (GEPC_EEPROM_BLOCKS * GEPC_EEPROM_BLOCK)

static u8  g_eeprom[GEPC_EEPROM_BYTES];
static int g_eeprom_loaded;

static void eeprom_path(char *out, size_t n)
{
    snprintf(out, n, "%s/ge007.eep", platformGetDataPath());
}

static void eeprom_load(void)
{
    char path[1024];
    FILE *f;

    if (g_eeprom_loaded) {
        return;
    }
    g_eeprom_loaded = 1;
    memset(g_eeprom, 0, sizeof(g_eeprom));

    eeprom_path(path, sizeof(path));
    f = fopen(path, "rb");
    if (!f) {
        return;   /* no save yet, which is not an error */
    }
    fread(g_eeprom, 1, sizeof(g_eeprom), f);
    fclose(f);
}

static void eeprom_store(void)
{
    char path[1024];
    FILE *f;

    eeprom_path(path, sizeof(path));
    f = fopen(path, "wb");
    if (!f) {
        platformLog("could not write the save file: %s", path);
        return;
    }
    fwrite(g_eeprom, 1, sizeof(g_eeprom), f);
    fclose(f);
}

s32 osEepromProbe(OSMesgQueue *mq)
{
    (void)mq;
    eeprom_load();
    return EEPROM_TYPE_16K;
}

s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer)
{
    (void)mq;
    eeprom_load();

    /* No range check on `address`: it is a u8 and there are exactly 256
     * blocks, so every value it can hold is valid. The compiler points this
     * out, and it is right -- but see the long variants below, where the
     * computed block index genuinely can run past the end. */
    if (!buffer) {
        return -1;
    }
    memcpy(buffer, g_eeprom + (unsigned)address * GEPC_EEPROM_BLOCK,
           GEPC_EEPROM_BLOCK);
    return 0;
}

s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer)
{
    (void)mq;
    eeprom_load();

    if (!buffer) {
        return -1;
    }
    memcpy(g_eeprom + (unsigned)address * GEPC_EEPROM_BLOCK, buffer,
           GEPC_EEPROM_BLOCK);
    /* Written through immediately. A crash between blocks would otherwise
     * lose a save the player believes was made. */
    eeprom_store();
    return 0;
}

/* The block index is computed here, so unlike the single-block calls above it
 * really can run off the end -- and casting it back to u8 would wrap it round
 * to the start, quietly corrupting the beginning of the save rather than
 * failing. Checked in int before any narrowing. */
static int eeprom_range_ok(u8 address, int length)
{
    int blocks;

    if (length <= 0 || (length % GEPC_EEPROM_BLOCK) != 0) {
        return 0;
    }
    blocks = length / GEPC_EEPROM_BLOCK;
    return ((int)address + blocks) <= GEPC_EEPROM_BLOCKS;
}

s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int length)
{
    int done = 0;

    if (!buffer || !eeprom_range_ok(address, length)) {
        return -1;
    }
    while (done < length) {
        if (osEepromRead(mq, (u8)(address + done / GEPC_EEPROM_BLOCK),
                         buffer + done) != 0) {
            return -1;
        }
        done += GEPC_EEPROM_BLOCK;
    }
    return 0;
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int length)
{
    int done = 0;

    if (!buffer || !eeprom_range_ok(address, length)) {
        return -1;
    }
    while (done < length) {
        if (osEepromWrite(mq, (u8)(address + done / GEPC_EEPROM_BLOCK),
                          buffer + done) != 0) {
            return -1;
        }
        done += GEPC_EEPROM_BLOCK;
    }
    return 0;
}

/* --------------------------------------------- video interface */

/* Most of this drives nothing: the renderer owns the framebuffer and its own
 * swap, so these exist mainly so the game's video bookkeeping links and runs.
 * osCreateViManager is the exception -- see below. */

static void *g_framebuffer;

/*
 * The game asks for a video manager once, during startup, before it draws
 * anything. That makes it the natural place to open the window: it is the
 * moment the game first says it wants a display, it happens on the game's own
 * schedule rather than the port's, and it is early enough that the first
 * graphics task already has somewhere to go.
 *
 * The window is deliberately larger than the N64's 320x240 framebuffer;
 * gfxGLSetOutputSize maps the game's coordinates across. GE007_SCALE overrides
 * the multiplier for anyone who wants a different size.
 */
void osCreateViManager(OSPri pri)
{
    const char *env;
    int scale = 3;

    (void)pri;

    if (videoIsReady()) {
        return;
    }

    env = getenv("GE007_SCALE");
    if (env) {
        int v = atoi(env);
        if (v >= 1 && v <= 8) {
            scale = v;
        } else {
            platformLog("GE007_SCALE=%s out of range 1-8, using %d", env, scale);
        }
    }

    if (videoInit(320 * scale, 240 * scale, "GoldenEye 007") != 0) {
        /* Fatal rather than silent: without a window the game runs blind,
         * building display lists that go nowhere, and the first sign of
         * trouble would be a black screen with no explanation. */
        platformPanic("ge007: could not open the game window");
    }
}
void  osViSetMode(OSViMode *m)                { (void)m; }
void  osViSetEvent(OSMesgQueue *mq, OSMesg m, u32 n) { (void)mq; (void)m; (void)n; }
void  osViSetSpecialFeatures(u32 f)           { (void)f; }
void  osViSetXScale(f32 s)                    { (void)s; }
void  osViSetYScale(f32 s)                    { (void)s; }
void  osViBlack(u8 b)                         { (void)b; }
void  osViRepeatLine(u8 b)                    { (void)b; }
void  osViSwapBuffer(void *fb)                { g_framebuffer = fb; }
void *osViGetCurrentFramebuffer(void)         { return g_framebuffer; }
void *osViGetNextFramebuffer(void)            { return g_framebuffer; }

/* ------------------------------------------------------- RDP (stubs) */

void osDpSetStatus(u32 s)                     { (void)s; }
void osDpGetCounters(u32 *c)                  { if (c) { *c = 0; } }
s32  osDpSetNextBuffer(void *p, u64 n)        { (void)p; (void)n; return 0; }

/* ------------------------------------------------------------ PI misc */

u32 osPiGetStatus(void)                       { return 0; }

s32 osPiReadIo(u32 devAddr, u32 *data)
{
    return osPiRawReadIo(devAddr, data);
}

s32 osPiWriteIo(u32 devAddr, u32 data)
{
    return osPiRawWriteIo(devAddr, data);
}

/* ------------------------------------------- hardware-only internals */

void osInitialize(void)
{
    /* Boot-time hardware bring-up. Everything it would configure -- the count
     * register, the TLB, the interrupt tables -- either does not exist here or
     * is already set up by the port before the game runs. */
}

void osUnmapTLB(s32 index)                    { (void)index; }
void osUnmapTLBAll(void)                      { }
u32  __osGetTLBHi(void)                       { return 0; }
u32  __osGetFpcCsr(void)                      { return 0; }
u32  __osSetFpcCsr(u32 v)                     { (void)v; return 0; }

s32 osDiskExist(void)                         { return 0; }   /* no 64DD */

/* Serial interface: the controller path above replaces it wholesale. */
u8  __osPfsPifRam[64];
u8  __osContLastCmd;

s32 __osSiRawStartDma(s32 dir, void *dramAddr)
{
    (void)dir; (void)dramAddr;
    return 0;
}

s32  __osSiGetAccess(void)                    { return 0; }
s32  __osSiRelAccess(void)                    { return 0; }
u8   __osContAddressCrc(u16 addr)             { (void)addr; return 0; }

s32 __osContRamRead(OSMesgQueue *mq, int channel, u16 address, u8 *buffer)
{
    (void)mq; (void)channel; (void)address; (void)buffer;
    return 0;
}

s32 __osContRamWrite(OSMesgQueue *mq, int channel, u16 address, u8 *buffer,
                     int force)
{
    (void)mq; (void)channel; (void)address; (void)buffer; (void)force;
    return 0;
}

s32 osPfsInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    (void)mq; (void)pfs; (void)channel;
    /* No controller pak. The game falls back to EEPROM saves. */
    return PFS_ERR_NOPACK;
}

/* --------------------------------------------------------- libm constant */

/*
 * src/libultra/gu/cosf.c returns this for an out-of-range argument. On IDO it
 * came from libm; here it is just the quiet NaN it names.
 */
float __libm_qnan_f = (float)(0.0 / 0.0);
