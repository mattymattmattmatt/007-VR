/*
 * romdata.h - the player's own ROM, mapped into memory.
 *
 * The port does not reimplement GoldenEye's file table, its segment layout or
 * its rz decompression. The game already knows how to read all of that; it
 * just expects to do so over PI DMA from a cartridge. So romdata loads the
 * player's ROM into memory and libultra.c's osPiStartDma reads out of it,
 * leaving every asset path in the game exactly as it was.
 *
 * No assets ship with this repository. The player supplies their own ROM.
 */
#ifndef GEPC_ROMDATA_H
#define GEPC_ROMDATA_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum gepc_rom_version {
    GEPC_ROM_UNKNOWN = 0,
    GEPC_ROM_US,
    GEPC_ROM_JP,
    GEPC_ROM_EU
} gepc_rom_version;

/* Cartridge domain 1 base in the N64's physical address map. */
#define GEPC_CART_BASE 0x10000000u

/* Loads a ROM. Pass NULL to search the data directory for the usual names.
 * Returns 0 on success. On failure the reason has already been logged in
 * terms a player can act on. */
int romdataLoad(const char *path);
void romdataUnload(void);
int romdataIsLoaded(void);

gepc_rom_version romdataGetVersion(void);
const char      *romdataGetVersionName(void);
const char      *romdataGetSha1(void);
unsigned         romdataGetSize(void);
const unsigned char *romdataGetBase(void);

/* Reads from a cartridge offset. Returns 0 on success, -1 if the range falls
 * outside the ROM. */
int romdataRead(unsigned offset, void *dst, unsigned size);

/* Turns an N64 CPU address into a cartridge offset, undoing KSEG0/KSEG1
 * segmentation. Returns -1 if the address is not in cartridge space. */
long romdataAddrToOffset(unsigned addr);

/* Byte order of a ROM image, decided from its 4-byte magic. Exposed for the
 * tests; romdataLoad normalises everything to big-endian z64 on load. */
typedef enum gepc_rom_format {
    GEPC_ROM_FORMAT_INVALID = 0,
    GEPC_ROM_FORMAT_Z64,   /* big endian, as the console reads it */
    GEPC_ROM_FORMAT_V64,   /* 16-bit byte-swapped */
    GEPC_ROM_FORMAT_N64    /* 32-bit little endian */
} gepc_rom_format;

gepc_rom_format romdataDetectFormat(const unsigned char *data, unsigned size);
void            romdataNormalise(unsigned char *data, unsigned size,
                                 gepc_rom_format format);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_ROMDATA_H */
