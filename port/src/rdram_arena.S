/*
 * The RDRAM arena, reserved at link time.
 *
 * The arena has to live at exactly 0x800000 -- see the long comment in
 * rdram.c for why that address and no other -- and the obvious way to get it
 * is mmap with MAP_FIXED_NOREPLACE. That works almost always, which is worse
 * than not working: the kernel randomises the start of the brk heap to
 * somewhere above the end of the executable's data segment, and the range it
 * can reach covers 0x800000-0x1000000. Measured on the machine this was
 * written on, the heap landed inside the arena about eight times in a
 * thousand -- the mapping is then correctly refused and the game will not
 * start, with nothing to distinguish that run from the many before it.
 *
 * Reserving the range in the executable's own image removes the race instead
 * of narrowing it. The kernel sets the initial brk from the end of the last
 * PT_LOAD segment, so once this section is part of the image the heap is
 * placed above it by construction and can never be randomised into the arena.
 *
 * It costs nothing on disk: @nobits means the section occupies address space
 * and no file space, exactly like .bss, and the pages arrive demand-zeroed --
 * which is also what the game expects, since hardware RDRAM comes up zeroed.
 *
 * The address itself is applied at link time with
 * --section-start=.gepc_rdram=0x800000; see port/CMakeLists.txt. rdram.c
 * checks at startup that the symbol really did land there.
 */
    .section .gepc_rdram, "aw", @nobits
    .globl gepcRdramArena
    .p2align 12
gepcRdramArena:
    .space 0x800000

/* Mark the stack non-executable. Without this the linker warns, and on some
 * toolchains assumes an executable stack for the whole program. */
    .section .note.GNU-stack, "", @progbits
