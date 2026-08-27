#!/usr/bin/env python3
"""Run the DSP regression: the firmware's own C code against recorded fixtures.

The rates published for this project were produced by a Python reference
implementation and checked against human breath counts. The C in
components/mr60_breathing/dsp.c is a port of that reference, and this script is
what stops the port drifting away from it. It builds nothing - run `make test`,
which builds tools/replay/replay first.

Exit code 0 when every case passes, 1 otherwise.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPLAY = ROOT / "tools" / "replay" / "replay"
FIXTURES = ROOT / "tests" / "fixtures"
EXPECTED = ROOT / "tests" / "expected.json"


def run_case(case: dict) -> dict:
    fixture = FIXTURES / case["fixture"]
    if not fixture.exists():
        raise SystemExit(f"missing fixture: {fixture}")
    cmd = [str(REPLAY), str(fixture), *case.get("args", []), "--json"]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def check(case: dict, got: dict, tol: dict) -> list[str]:
    exp = case["expect"]
    bad = []

    if bool(got["stable"]) != bool(exp["stable"]):
        bad.append(f"stable: expected {exp['stable']}, got {bool(got['stable'])}")

    if exp.get("status") and got["status"] != exp["status"]:
        bad.append(f"status: expected {exp['status']!r}, got {got['status']!r}")

    if exp.get("rate_bpm") is not None:
        d = abs(got["rate_bpm"] - exp["rate_bpm"])
        if d > tol["rate_bpm"]:
            bad.append(f"rate: expected {exp['rate_bpm']:.1f} +/-"
                       f"{tol['rate_bpm']:.1f}, got {got['rate_bpm']:.1f}"
                       f" (off by {d:.2f})")

    if exp.get("stability_pct") is not None:
        d = abs(got["stability_pct"] - exp["stability_pct"])
        if d > tol["stability_pct"]:
            bad.append(f"stability: expected {exp['stability_pct']:.0f} +/-"
                       f"{tol['stability_pct']:.0f}, got "
                       f"{got['stability_pct']:.0f}")
    return bad


def main() -> int:
    if not REPLAY.exists() and not REPLAY.with_suffix(".exe").exists():
        raise SystemExit(f"{REPLAY} not built - run `make` first")

    spec = json.loads(EXPECTED.read_text(encoding="utf-8"))
    tol = spec["tolerance"]

    width = max(len(c["name"]) for c in spec["cases"])
    failures = 0

    print(f"{'case':<{width}}  {'rate':>7} {'ref':>6} {'stab':>6} "
          f"{'snr':>6}  verdict")
    print("-" * (width + 40))

    for case in spec["cases"]:
        got = run_case(case)
        bad = check(case, got, tol)
        exp = case["expect"]

        ref = f"{exp['rate_bpm']:.1f}" if exp.get("rate_bpm") is not None else "-"
        verdict = "STABLE" if got["stable"] else "no pattern"
        mark = "ok " if not bad else "FAIL"
        print(f"{case['name']:<{width}}  {got['rate_bpm']:>7.1f} {ref:>6} "
              f"{got['stability_pct']:>5.0f}% {got['snr_db']:>6.1f}  "
              f"{verdict:<11} {mark}")

        counted = case.get("counted_bpm")
        if counted and got["stable"]:
            err = got["rate_bpm"] - counted
            print(f"{'':<{width}}  against a human count of {counted}: "
                  f"{err:+.1f} /min")

        for line in bad:
            print(f"{'':<{width}}  -> {line}")
            failures += 1

    print()
    if failures:
        print(f"{failures} check(s) failed")
        return 1
    print(f"all {len(spec['cases'])} cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
