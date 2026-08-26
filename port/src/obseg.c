#include "obseg.h"

#include "platform.h"
#include "romdata.h"

#include <string.h>

#include <ultra64.h>
#include "game/ob.h"

/* Defined by the game, in src/game/ob.c. */
extern fileentry file_resource_table[];
extern s32       file_entry_max;

static unsigned g_unbound;

/*
 * The table names a file the way the game refers to it, which is not quite
 * how the manifest does. "bg/bg_sev_all_p.seg" is the same file the manifest
 * calls "bg/bg_sev_all_p", and "CarmourguardZ" is the one it calls
 * "chr/CarmourguardZ" -- some table entries carry a directory and some do
 * not. So a match is either the whole relative path or just the last
 * component, after dropping the .seg the table adds.
 */
static int names_match(const char *table_name, const char *manifest_name)
{
    const char *slash;
    size_t len = strlen(table_name);

    if (len > 4 && strcmp(table_name + len - 4, ".seg") == 0) {
        len -= 4;
    }

    if (strlen(manifest_name) == len &&
        strncmp(manifest_name, table_name, len) == 0) {
        return 1;
    }

    slash = strrchr(manifest_name, '/');
    if (slash) {
        slash++;
        if (strlen(slash) == len && strncmp(slash, table_name, len) == 0) {
            return 1;
        }
    }
    return 0;
}

static const gepc_rom_file *find(const char *name)
{
    unsigned i;

    for (i = 0; i < gepcRomFileCount; i++) {
        if (names_match(name, gepcRomFiles[i].name)) {
            return &gepcRomFiles[i];
        }
    }
    return NULL;
}

int gepcObsegBindFileTable(void)
{
    s32 i;
    int bound = 0;

    g_unbound = 0;

    if (file_entry_max <= 0) {
        return -1;
    }
    if (!romdataIsLoaded()) {
        platformLog("obseg: no ROM loaded; file table left unbound");
        return -1;
    }

    /* Entry 0 is NULLFILE and has no name. */
    for (i = 1; i < file_entry_max; i++) {
        fileentry *e = &file_resource_table[i];
        const gepc_rom_file *f;

        if (!e->filename || !e->filename[0]) {
            continue;
        }

        f = find(e->filename);
        if (!f) {
            /* Left at zero deliberately: the game reads that as "not in the
             * cartridge" and takes its own path, which is a defined outcome
             * rather than a read from address zero. */
            e->hw_address = NULL;
            g_unbound++;
            continue;
        }

        if (f->offset > romdataGetSize() ||
            f->size > romdataGetSize() - f->offset) {
            /* The manifest describes a different ROM than the one loaded.
             * Saying so is worth more than a silent bad address. */
            platformLog("obseg: %s lies outside this ROM (offset %u size %u, "
                        "ROM %u bytes)", e->filename, f->offset, f->size,
                        romdataGetSize());
            e->hw_address = NULL;
            g_unbound++;
            continue;
        }

        /* A cartridge address. osPiStartDma masks the KSEG bits off and
         * subtracts GEPC_CART_BASE, which lands back on f->offset. */
        e->hw_address = (u8 *)(uintptr_t)(0xB0000000u + f->offset);
        bound++;
    }

    platformLog("obseg: %d of %d file table entries bound to the ROM"
                "%s%u unmatched%s",
                bound, (int)file_entry_max - 1,
                g_unbound ? " (" : "", g_unbound, g_unbound ? ")" : "");
    return bound;
}

unsigned gepcObsegUnboundCount(void) { return g_unbound; }
