/*
 * main.c - where the port starts, standing in for boot.s.
 *
 * On the console the reset vector lands in boot.s, which sets up the stack
 * and calls init(). init() is described in src/init.c as "the real main entry
 * point"; everything before it is bare hardware bring-up in assembly that the
 * port neither compiles nor needs.
 *
 * So this does what boot.s did -- get the machine into a state where the game
 * can run -- and then calls the same init() the cartridge did. From that point
 * the game drives itself: init() starts the main thread, which runs mainproc,
 * which builds display lists that reach the renderer through the intercepted
 * SP task.
 */
#include "platform.h"
#include "rdram.h"
#include "obseg.h"
#include "romdata.h"
#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* src/init.c. Declared here rather than including boot.h, so the port does
 * not pull the game's headers into its own entry point. */
extern void init(void);

/*
 * No game data ships with this port and none ever will: the player supplies
 * the cartridge they own. Everything the game draws and plays comes out of
 * it at run time through romdata.c.
 */
static const char *find_rom(int argc, char **argv)
{
    const char *env;

    if (argc > 1 && argv[1] && argv[1][0]) {
        return argv[1];
    }
    env = getenv("GE007_ROM");
    if (env && env[0]) {
        return env;
    }
    return NULL;
}

static void explain_missing_rom(const char *tried)
{
    fprintf(stderr,
        "\nge007: no ROM to run.\n\n"
        "This port ships no game data. Supply the GoldenEye 007 cartridge you\n"
        "own, as a .z64, .v64 or .n64 image:\n\n"
        "    ge007 /path/to/goldeneye.z64\n"
        "    GE007_ROM=/path/to/goldeneye.z64 ge007\n\n");
    if (tried) {
        fprintf(stderr, "Tried: %s\n\n", tried);
    }
}

int main(int argc, char **argv)
{
    const char *rom = find_rom(argc, argv);

    platformInit();

    /* The arena has to exist before anything the game allocates, and it
     * checks the executable's own linkage on the way -- see
     * gepcAssertLowMemory in rdram.c. */
    if (rdramInit() != 0) {
        fprintf(stderr, "ge007: could not reserve the low memory arena.\n");
        return 1;
    }

    if (!rom) {
        explain_missing_rom(NULL);
        return 2;
    }
    if (romdataLoad(rom) != 0) {
        explain_missing_rom(rom);
        return 2;
    }

    platformLog("rom: %s (%u MB), sha1 %s",
                romdataGetVersionName(), romdataGetSize() / (1024u * 1024u),
                romdataGetSha1());

    /*
     * The game's file table has to know where its files live in the ROM
     * before anything asks for one. On the console the linker did this; here
     * it happens now, once the ROM is open and before the game starts.
     */
    if (gepcObsegBindFileTable() < 0) {
        fprintf(stderr, "ge007: could not bind the file table to the ROM.\n");
        return 3;
    }

    /* From here the game is in charge. init() starts the main thread and
     * returns; the window opens when the game asks for a video manager. */
    init();

    /* The game's threads do the work. This one owns the window, because SDL
     * wants events pumped from the thread that created it, and it is also
     * where a close request has to be noticed. */
    while (!videoPumpEvents()) {
        platformSleepNs(2 * 1000 * 1000);   /* 2ms: responsive, not a spin */
    }

    platformLog("shutting down after %u frames", videoFrameCount());
    videoShutdown();
    romdataUnload();
    rdramShutdown();
    platformShutdown();
    return 0;
}
