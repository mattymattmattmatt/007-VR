#include "romdata.h"

#include "platform.h"
#include "sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Checksums of the three known good ROMs, copied from the ge007.*.sha1 files
 * at the top of the repository. These are only identifiers: a wrong ROM
 * otherwise fails much later, deep in an asset read, with no clue why. */
static const struct {
    gepc_rom_version version;
    const char      *name;
    const char      *sha1;
    const char      *filename;
} k_known_roms[] = {
    { GEPC_ROM_US, "NTSC-U", "abe01e4aeb033b6c0836819f549c791b26cfde83", "ge007.u.z64" },
    { GEPC_ROM_JP, "NTSC-J", "2a5dade32f7fad6c73c659d2026994632c1b3174", "ge007.j.z64" },
    { GEPC_ROM_EU, "PAL",    "167c3c433dec1f1eb921736f7d53fac8cb45ee31", "ge007.e.z64" }
};

#define K_KNOWN_ROM_COUNT ((int)(sizeof(k_known_roms) / sizeof(k_known_roms[0])))

static unsigned char   *g_rom;
static unsigned         g_rom_size;
static gepc_rom_version g_version;
static const char      *g_version_name = "unknown";
static char             g_sha1_hex[41];

/* ------------------------------------------------------------- byte order */

gepc_rom_format romdataDetectFormat(const unsigned char *data, unsigned size)
{
    if (!data || size < 4) {
        return GEPC_ROM_FORMAT_INVALID;
    }

    /* Every N64 ROM starts with the same header word; which permutation of it
     * appears on disk is what tells the three dump formats apart. */
    if (data[0] == 0x80 && data[1] == 0x37 && data[2] == 0x12 && data[3] == 0x40) {
        return GEPC_ROM_FORMAT_Z64;
    }
    if (data[0] == 0x37 && data[1] == 0x80 && data[2] == 0x40 && data[3] == 0x12) {
        return GEPC_ROM_FORMAT_V64;
    }
    if (data[0] == 0x40 && data[1] == 0x12 && data[2] == 0x37 && data[3] == 0x80) {
        return GEPC_ROM_FORMAT_N64;
    }
    return GEPC_ROM_FORMAT_INVALID;
}

void romdataNormalise(unsigned char *data, unsigned size, gepc_rom_format format)
{
    unsigned i;

    if (!data) {
        return;
    }

    switch (format) {
    case GEPC_ROM_FORMAT_V64:
        /* Swap each 16-bit pair. */
        for (i = 0; i + 1 < size; i += 2) {
            unsigned char t = data[i];
            data[i] = data[i + 1];
            data[i + 1] = t;
        }
        break;

    case GEPC_ROM_FORMAT_N64:
        /* Reverse each 32-bit word. */
        for (i = 0; i + 3 < size; i += 4) {
            unsigned char t0 = data[i];
            unsigned char t1 = data[i + 1];
            data[i] = data[i + 3];
            data[i + 1] = data[i + 2];
            data[i + 2] = t1;
            data[i + 3] = t0;
        }
        break;

    case GEPC_ROM_FORMAT_Z64:
    default:
        break;
    }
}

/* ------------------------------------------------------------------ load */

static unsigned char *read_whole_file(const char *path, unsigned *out_size)
{
    FILE *f;
    long size;
    unsigned char *buf;

    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = (unsigned char *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    *out_size = (unsigned)size;
    return buf;
}

static void identify(void)
{
    int i;

    gepc_sha1_hex(g_rom, g_rom_size, g_sha1_hex);

    g_version = GEPC_ROM_UNKNOWN;
    g_version_name = "unknown";

    for (i = 0; i < K_KNOWN_ROM_COUNT; i++) {
        if (strcmp(g_sha1_hex, k_known_roms[i].sha1) == 0) {
            g_version = k_known_roms[i].version;
            g_version_name = k_known_roms[i].name;
            return;
        }
    }
}

int romdataLoad(const char *path)
{
    char candidate[1024];
    unsigned char *data = NULL;
    unsigned size = 0;
    gepc_rom_format format;
    int i;

    romdataUnload();

    if (path && *path) {
        data = read_whole_file(path, &size);
        if (!data) {
            platformLog("could not open ROM: %s", path);
            return -1;
        }
    } else {
        for (i = 0; i < K_KNOWN_ROM_COUNT && !data; i++) {
            snprintf(candidate, sizeof(candidate), "%s/%s",
                     platformGetDataPath(), k_known_roms[i].filename);
            data = read_whole_file(candidate, &size);
            if (data) {
                path = k_known_roms[i].filename;
            }
        }
        if (!data) {
            platformLog("no GoldenEye ROM found in '%s'.",
                        platformGetDataPath());
            platformLog("place your own ROM there as one of: %s, %s, %s",
                        k_known_roms[0].filename,
                        k_known_roms[1].filename,
                        k_known_roms[2].filename);
            return -1;
        }
    }

    format = romdataDetectFormat(data, size);
    if (format == GEPC_ROM_FORMAT_INVALID) {
        platformLog("'%s' does not look like an N64 ROM (bad header magic).",
                    path);
        free(data);
        return -1;
    }
    if (format != GEPC_ROM_FORMAT_Z64) {
        platformLog("ROM is %s format; converting to big-endian in memory.",
                    format == GEPC_ROM_FORMAT_V64 ? "v64" : "n64");
        romdataNormalise(data, size, format);
    }

    g_rom = data;
    g_rom_size = size;
    identify();

    if (g_version == GEPC_ROM_UNKNOWN) {
        /* Not fatal. A romhack or a different revision may well work, and
         * refusing outright would be unhelpful; but the player should know
         * why things break if they do. */
        platformLog("warning: unrecognised ROM (sha1 %s).", g_sha1_hex);
        platformLog("expected one of the NTSC-U, NTSC-J or PAL releases; "
                    "continuing anyway.");
    } else {
        platformLog("loaded %s ROM, %u bytes (sha1 %s)",
                    g_version_name, g_rom_size, g_sha1_hex);
    }
    return 0;
}

void romdataUnload(void)
{
    free(g_rom);
    g_rom = NULL;
    g_rom_size = 0;
    g_version = GEPC_ROM_UNKNOWN;
    g_version_name = "unknown";
    g_sha1_hex[0] = '\0';
}

int romdataIsLoaded(void)                { return g_rom != NULL; }
gepc_rom_version romdataGetVersion(void) { return g_version; }
const char *romdataGetVersionName(void)  { return g_version_name; }
const char *romdataGetSha1(void)         { return g_sha1_hex; }
unsigned romdataGetSize(void)            { return g_rom_size; }
const unsigned char *romdataGetBase(void){ return g_rom; }

/* --------------------------------------------------------------- reading */

long romdataAddrToOffset(unsigned addr)
{
    unsigned phys;

    /* KSEG0 (0x80000000) and KSEG1 (0xA0000000) are both windows onto the
     * same physical memory; masking off the top three bits is how the CPU
     * itself resolves them. */
    phys = addr & 0x1FFFFFFFu;

    if (phys < GEPC_CART_BASE) {
        return -1;
    }
    return (long)(phys - GEPC_CART_BASE);
}

int romdataRead(unsigned offset, void *dst, unsigned size)
{
    if (!g_rom || !dst) {
        return -1;
    }

    /* Checked as a sum in 64-bit so a huge size cannot wrap past the end and
     * look in range. */
    if ((unsigned long long)offset + (unsigned long long)size
        > (unsigned long long)g_rom_size) {
        platformLog("ROM read out of range: offset 0x%x size 0x%x (rom 0x%x)",
                    offset, size, g_rom_size);
        return -1;
    }

    memcpy(dst, g_rom + offset, size);
    return 0;
}
