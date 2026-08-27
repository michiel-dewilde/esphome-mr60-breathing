# Raw capture

Recording raw radar data from a deployed device, so analysis can continue on
data taken in place rather than only from a USB-tethered bench.

## Why it costs nothing when you are not using it

Nothing is serialised until a client actually connects. With nobody attached,
the per-tile cost is a single boolean test and the only standing cost is one
non-blocking `accept()` per loop.

That restraint matters more here than usual: collection mode already runs the
radar UART at about 88 % of its capacity, so anything that adds work to the
main loop risks losing tile sets.

To remove the listening socket entirely, turn off the **Raw capture** switch, or
set `raw_capture_port: 0`.

## Recording

```sh
python tools/rawcapture.py breathing-monitor.local 120 capture.csv
```

Arguments are host, seconds (0 records until Ctrl-C) and output file. Add
`--port` if you moved it off 6060.

The tool reports the achieved set rate. It should be close to 4.88 Hz; below
4.7 it warns, because that means sets were lost either on the radar link or in
transit.

## Two modes

Set with `raw_capture_mode` in the YAML.

| mode | rate | contents |
|---|---|---|
| `phase` (default) | ~330 B/s | one CSV line per tile set: `t_us` then `re,im` for each of the 16 range rows |
| `tiles` | ~20 kB/s | one `R,<t_us>,<type>,<len>,0,<hex>` line per radar frame — the untouched payload |

**Neither format is new.** `phase` is exactly the CSV the regression fixtures
use, and `tiles` is the line format the original USB bridge produced. A capture
therefore drops straight into the existing tooling with nothing to write:

```sh
./tools/replay/replay capture.csv          # same C the device runs
```

`phase` is enough to reproduce every analysis the component performs — it is
the zero-Doppler bin of each range row, which is where the whole pipeline
starts. Reach for `tiles` only when you want the full range-Doppler picture,
for instance to investigate something the current analysis throws away.

## Adding a capture to the regression

A capture in `phase` mode can be dropped into `tests/fixtures/` and given an
entry in `tests/expected.json`. If you have a trustworthy human count to go
with it, put that in `counted_bpm` — the harness prints the error against it,
which is how the accuracy figures in the README were arrived at.

## Triggering from Home Assistant

The device does not need to be involved. Run the capture wherever Python and
network access are convenient — a `shell_command` on the Home Assistant host
works:

```yaml
shell_command:
  capture_breathing: >-
    python3 /config/scripts/rawcapture.py breathing-monitor.local 120
    /config/captures/{{ now().strftime('%Y%m%d_%H%M%S') }}.csv
```

```yaml
script:
  capture_breathing:
    alias: Record two minutes of raw radar
    sequence:
      - action: shell_command.capture_breathing
```

Only one client is served at a time. A second connection is accepted and closed
immediately rather than queued, so a forgotten session cannot silently block a
new one.
