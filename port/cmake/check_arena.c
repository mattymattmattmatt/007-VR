/*
 * Configure-time check for the link-time RDRAM arena.
 *
 * Linking is not enough to prove this works: an assembler that accepts
 * @nobits and a linker that accepts --section-start can still between them
 * put the section somewhere else. So this runs, writes to both ends of the
 * reservation, and reports whether the symbol actually landed on the address
 * it was asked for.
 */
extern unsigned char gepcRdramArena[];

int main(void)
{
    unsigned long long a = (unsigned long long)(unsigned long)gepcRdramArena;

    gepcRdramArena[0] = 1;
    gepcRdramArena[0x800000u - 1u] = 2;

    if (a != 0x800000ull) {
        return 1;
    }
    return (gepcRdramArena[0] == 1 && gepcRdramArena[0x800000u - 1u] == 2)
           ? 0 : 1;
}
