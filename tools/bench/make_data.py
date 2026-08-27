#!/usr/bin/env python3
"""Regenerate src/bench_data.h from a fixture.

The benchmark embeds a real 60 s recording rather than synthetic noise, so the
timing reflects the actual workload and the device result can be compared
against the host regression on identical input.

Usage: python tools/bench/make_data.py tests/fixtures/cat63_count.csv
"""
import io
import os
import sys

src = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/cat63_count.csv"
dst = os.path.join(os.path.dirname(__file__), "src", "bench_data.h")

rows = []
for line in io.open(src, encoding="utf-8"):
    if line.startswith("#") or not line.strip():
        continue
    p = line.strip().split(",")
    rows.append((int(p[0]), [int(float(v)) for v in p[1:]]))

n, t0 = len(rows), rows[0][0]
with io.open(dst, "w", encoding="utf-8", newline="\n") as out:
    out.write("/* Generated from %s by tools/bench/make_data.py.\n" % src)
    out.write(" * 60 s of a resting cat at 63 cm. The reference implementation reports\n")
    out.write(" * 28.8 /min at 67 %% stability on this window, and a human counted 28.\n")
    out.write(" */\n#ifndef BENCH_DATA_H\n#define BENCH_DATA_H\n#include <stdint.h>\n\n")
    out.write("#define BENCH_SETS %d\n\n" % n)
    out.write("static const uint32_t bench_dt_us[BENCH_SETS] = {\n")
    for i in range(0, n, 12):
        out.write("  " + ",".join(str(rows[j][0] - t0)
                                  for j in range(i, min(i + 12, n))) + ",\n")
    out.write("};\n\nstatic const int16_t bench_iq[BENCH_SETS][32] = {\n")
    for _, v in rows:
        out.write("  {" + ",".join(str(x) for x in v) + "},\n")
    out.write("};\n\n#endif\n")
print("%s: %d sets, %d kB" % (dst, n, os.path.getsize(dst) // 1024))
