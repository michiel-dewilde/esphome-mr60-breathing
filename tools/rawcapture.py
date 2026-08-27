#!/usr/bin/env python3
"""Record raw radar data from a deployed device over the network.

The point of this is that a recording taken through the finished sensor drops
straight into the same analysis that produced the published numbers - no
tethering the board to a laptop, and no new file format to teach anything
about.

Two modes, decided by the device's configuration:

  phase   one CSV line per tile set: t_us, then re,im for each of the 16 range
          rows. About 330 B/s. This is the format tests/fixtures/ uses, so a
          capture can be dropped in there and replayed with tools/replay.
  tiles   one "R,<t_us>,<type>,<len>,0,<hex>" line per radar frame, the
          untouched payload. About 20 kB/s.

Usage:
    python tools/rawcapture.py breathing-monitor.local 120 capture.csv
    python tools/rawcapture.py 192.168.1.42 60 capture.log --port 6060

The duration is in seconds; 0 records until interrupted.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("host", help="device hostname or address")
    ap.add_argument("seconds", type=float, nargs="?", default=60.0,
                    help="how long to record; 0 records until Ctrl-C")
    ap.add_argument("output", nargs="?", default=None,
                    help="file to write; defaults to a timestamped name")
    ap.add_argument("--port", type=int, default=6060)
    args = ap.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=10)
    except OSError as e:
        print(f"could not connect to {args.host}:{args.port}: {e}",
              file=sys.stderr)
        return 1
    sock.settimeout(5.0)

    reader = sock.makefile("rb")
    header = reader.readline().decode("ascii", "replace").strip()
    if not header.startswith("#mr60raw"):
        print(f"unexpected greeting: {header!r}", file=sys.stderr)
        return 1
    mode = "tiles" if "mode=tiles" in header else "phase"

    out_path = args.output
    if out_path is None:
        stamp = time.strftime("%Y%m%d_%H%M%S")
        out_path = f"capture_{stamp}.{'log' if mode == 'tiles' else 'csv'}"

    print(f"connected to {args.host}:{args.port} ({mode} mode)")
    print(f"writing {out_path}"
          + (f" for {args.seconds:.0f} s" if args.seconds else " until Ctrl-C"))

    lines = 0
    first_t = last_t = None
    start = time.time()

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        if mode == "phase":
            f.write("# MR60BHA2 collection-mode capture: zero-Doppler bin per "
                    "range row.\n")
            f.write("# Columns: t_us, then re,im for channel A rows 0-7, then "
                    "channel C rows 0-7.\n")
        try:
            while True:
                if args.seconds and (time.time() - start) >= args.seconds:
                    break
                try:
                    raw = reader.readline()
                except socket.timeout:
                    print("timed out waiting for data", file=sys.stderr)
                    break
                if not raw:
                    print("device closed the connection", file=sys.stderr)
                    break
                line = raw.decode("ascii", "replace")
                f.write(line)
                lines += 1

                # First field is the timestamp in both formats, though the
                # tiles format puts an "R" in front of it.
                parts = line.split(",", 2)
                try:
                    t = int(parts[1] if parts[0] == "R" else parts[0])
                except (ValueError, IndexError):
                    continue
                if first_t is None:
                    first_t = t
                last_t = t
        except KeyboardInterrupt:
            print()

    sock.close()

    span = (last_t - first_t) / 1e6 if first_t is not None and last_t else 0.0
    if mode == "phase" and span > 0:
        rate = (lines - 1) / span
        print(f"{lines} sets, {span:.1f} s, {rate:.3f} Hz")
        if rate < 4.7:
            print("  the set rate is below 4.88 Hz - sets were lost, either on "
                  "the radar link or in transit")
    else:
        print(f"{lines} lines, {span:.1f} s")

    if mode == "phase":
        print(f"\nreplay it with:\n  ./tools/replay/replay {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
