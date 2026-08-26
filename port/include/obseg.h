/*
 * obseg.h - giving the game's file table its ROM addresses.
 *
 * file_resource_table maps each of the game's files to where it lives in the
 * cartridge: {file id, "bg/bg_sev_all_p.seg", &bg_sev_all_p_seg}. On the
 * console that third field was resolved by the linker, because ob_seg.s
 * .incbin'd every one of those files out of an extracted ROM and the link
 * placed them at known addresses.
 *
 * Nothing like that can happen here. No game data ships with this port, so a
 * fresh checkout has no files to .incbin and those 790 symbols do not exist.
 * They were never really data anyway -- the game only ever uses the field as
 * an address to hand to romCopy, which goes to PI DMA, which in this port
 * reads out of the player's loaded ROM.
 *
 * So the port supplies the addresses directly. scripts/filelist.u.csv already
 * records where every file sits in the ROM and how long it is -- that is what
 * tools/extractor uses to pull assets out -- and it is layout information
 * rather than any of the content. gen_rom_manifest.py turns the obseg rows of
 * it into the table below, and gepcObsegBindFileTable writes them in.
 *
 * One detail worth keeping: the game already treats a zero hw_address as
 * "this file is not in the cartridge" and takes a different path, so an entry
 * the manifest does not cover degrades the way the game expects instead of
 * reading from address zero.
 */
#ifndef GEPC_OBSEG_H
#define GEPC_OBSEG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gepc_rom_file {
    const char *name;    /* path under assets/obseg, without the extension */
    unsigned    offset;  /* byte offset into the ROM image */
    unsigned    size;    /* length in bytes */
} gepc_rom_file;

extern const gepc_rom_file gepcRomFiles[];
extern const unsigned      gepcRomFileCount;

/*
 * Fills in file_resource_table's addresses. Call once, after the ROM is
 * loaded and before the game starts. Returns the number of entries bound, or
 * -1 if the table could not be reached at all.
 */
int gepcObsegBindFileTable(void);

/* How many table entries the manifest did not cover on the last bind. */
unsigned gepcObsegUnboundCount(void);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_OBSEG_H */
