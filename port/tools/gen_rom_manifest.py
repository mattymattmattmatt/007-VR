#!/usr/bin/env python3
"""Turn the repository's ROM file list into a C table of offsets.

scripts/filelist.u.csv is the manifest tools/extractor uses to pull assets out
of a player's ROM: each row is `offset,size,path`. That is layout information
about the ROM -- where each file begins and how long it is -- and nothing of
the files' contents. It is exactly what the port needs, and it means the port
does not have to guess at the ROM's structure or ship any of it.

The rows this cares about are the ones under assets/obseg. On the console the
linker resolved those files' addresses into file_resource_table at build time,
from data .incbin'd out of an extracted ROM. A fresh checkout has none of that
data, so the port fills the same field in at startup instead -- see
port/src/obseg.c.
"""
import csv
import os
import sys

PREFIX = "assets/obseg/"


def rows(path):
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if len(row) < 3 or not row[0].strip().isdigit():
                continue
            offset, size, p = int(row[0]), int(row[1]), row[2].strip()
            if not p.startswith(PREFIX):
                continue
            rel = p[len(PREFIX):]
            if rel.endswith(".bin"):
                rel = rel[:-4]
            yield rel, offset, size


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_rom_manifest.py <filelist.csv> <out.c>\n")
        return 2

    entries = sorted(set(rows(argv[1])))
    os.makedirs(os.path.dirname(os.path.abspath(argv[2])), exist_ok=True)

    with open(argv[2], "w") as out:
        out.write('/* Generated from %s by port/tools/gen_rom_manifest.py.\n'
                  ' * Do not edit; edit the CSV or the generator. */\n'
                  '#include "obseg.h"\n\n'
                  'const gepc_rom_file gepcRomFiles[] = {\n'
                  % os.path.basename(argv[1]))
        for rel, offset, size in entries:
            out.write('    { "%s", %uu, %uu },\n' % (rel, offset, size))
        out.write('};\n\n'
                  'const unsigned gepcRomFileCount =\n'
                  '    (unsigned)(sizeof(gepcRomFiles) / sizeof(gepcRomFiles[0]));\n')

    sys.stderr.write("gen_rom_manifest: %d obseg files\n" % len(entries))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
