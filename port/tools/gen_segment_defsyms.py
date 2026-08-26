#!/usr/bin/env python3
"""Place the ROM segment symbols at their cartridge addresses.

ge007.ld gives each ROM segment four symbols -- _<name>SegmentRomStart and
RomEnd for where it sits in the cartridge, Start and End for where it lands in
RAM. The game takes their *addresses*: it DMAs from &_xSegmentRomStart and
sizes the transfer from &_xSegmentEnd - &_xSegmentStart, or from the gap
between two consecutive RomStarts. Nothing ever reads one as a value, which is
what makes this approach work.

So rather than define objects (whose addresses would be wherever the host
linker felt like putting them), these are emitted as `ld --defsym`, which
places a symbol at a literal address. No source change, no runtime lookup, and
the difference between two of them is the real size because the addresses are
the real addresses.

Offsets come from scripts/filelist.u.csv, the same manifest tools/extractor
uses -- layout, not content.
"""
import csv
import sys

ROM_BASE = 0xB0000000

# Segment name in ge007.ld -> the manifest row that begins it. Both lists run
# in ROM order and line up one to one, which is the cross-check: if a name were
# mismapped, its offset would fall out of sequence.
SEGMENT_FILE = {
    "fontdl":            "assets/ge007.u.117880.jfont_dl.bin",
    "jfontchardata":     "assets/ge007.u.117940.jfont_chardata.bin",
    "efontchardata":     "assets/ge007.u.123040.efont_chardata.bin",
    "animation_entries": "assets/animationtable_entries.bin",
    "animation_data":    "assets/animationtable_data.bin",
    "Globalimagetable":  "assets/ge007.u.29D160.Globalimagetable.bin",
    "rarewarelogo":      "assets/rarewarelogo.bin",
    "fontbankgothic":    "assets/font/fontBankGothic_kerning.bin",
    "fontzurichbold":    "assets/font/fontZurichBold_kerning.bin",
    "sfxctl":            "assets/music/sfx.ctl",
    "sfxtbl":            "assets/music/sfx.tbl",
    "instrumentsctl":    "assets/music/instruments.ctl",
    "instrumentstbl":    "assets/music/instruments.tbl",
    "musicsampletbl":    "assets/music/music.sbk",
}

# A segment's RAM size. Only the difference between Start and End is ever read,
# so the pair is emitted at 0 and this. These segments are stored uncompressed,
# so the ROM length is the RAM length.
RAM_SIZED = ("animation_data", "Globalimagetable", "rarewarelogo",
             "fontbankgothic", "fontzurichbold")


def load(path):
    rows = {}
    with open(path, newline="") as f:
        for r in csv.reader(f):
            if len(r) >= 3 and r[0].strip().isdigit():
                rows[r[2].strip()] = (int(r[0]), int(r[1]))
    return rows


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_segment_defsyms.py <filelist.csv> <out>\n")
        return 2

    rows = load(argv[1])
    out = []
    missing = []

    def defsym(name, value):
        out.append("--defsym=%s=0x%08X" % (name, value))

    for seg, path in SEGMENT_FILE.items():
        if path not in rows:
            missing.append("%s (%s)" % (seg, path))
            continue
        offset, size = rows[path]
        defsym("_%sSegmentRomStart" % seg, ROM_BASE + offset)
        defsym("_%sSegmentRomEnd" % seg, ROM_BASE + offset + size)
        if seg in RAM_SIZED:
            defsym("_%sSegmentStart" % seg, 0)
            defsym("_%sSegmentEnd" % seg, size)

    # The images segment begins where obseg ends. Deriving it rather than
    # naming a row, because no single row marks the boundary.
    obseg = [v for k, v in rows.items() if k.startswith("assets/obseg/")]
    if obseg:
        end = max(o + s for o, s in obseg)
        defsym("_imagesSegmentRomStart", ROM_BASE + ((end + 15) & ~15))

    # The attract-mode recordings, named directly after their files.
    for path, (offset, _size) in sorted(rows.items()):
        if path.startswith("assets/ramrom/"):
            name = path[len("assets/ramrom/"):-len(".bin")]
            defsym(name, ROM_BASE + offset)

    # title.c copies this span; the manifest calls it by the address it was
    # found at rather than by a name the game uses.
    if "assets/ge007.u.2A4D50.usedby7F008DE4.bin" in rows:
        offset, size = rows["assets/ge007.u.2A4D50.usedby7F008DE4.bin"]
        defsym("unknown2", ROM_BASE + offset)
        defsym("unknown2_end", ROM_BASE + offset + size)

    # Four that are not ROM addresses at all.
    #
    # _bssSegmentEnd is where the game's memory pool begins: boss.c takes its
    # address and hands the span from there to the TLB block to
    # mempCheckMemflagTokens. On the console that was the end of BSS; here it
    # is the base of the RDRAM arena, which rdram.c pins to exactly this
    # address -- it is the only base that clears the -no-pie image at 0x400000
    # while still ending by 0x1000000, and main.c checks the two agree at
    # startup, because a mismatch would hand the pool memory nothing has
    # mapped.
    defsym("_bssSegmentEnd", 0x00800000)

    # crash.c uses these to decide whether a faulted program counter looks
    # like a code address before it tries to disassemble around it. It is a
    # sanity check on a crash dump, not something the game depends on, so the
    # range is deliberately permissive rather than pretending to know where
    # the host put .text.
    defsym("_codeSegmentStart", 0x00000000)
    defsym("_codeSegmentEnd", 0x7FFFFFFF)

    # tlb_manage.c pages code in from here on a TLB miss. The port has no TLB
    # -- init.c's handler setup is guarded out, since installing one means
    # writing to physical address zero -- so this should never be reached.
    # Poisoned rather than plausible: romdataAddrToOffset rejects anything
    # below the cartridge base, so if that path does run it fails loudly
    # instead of DMAing from somewhere arbitrary.
    defsym("_gameSegmentRomStart", 0x00000000)

    with open(argv[2], "w") as f:
        f.write("\n".join(out) + "\n")

    sys.stderr.write("gen_segment_defsyms: %d symbols\n" % len(out))
    if missing:
        sys.stderr.write("  unmapped segments: %s\n" % ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
