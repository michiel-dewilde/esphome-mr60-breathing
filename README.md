# esphome-mr60-breathing

Breathing-rate sensing for Home Assistant on the Seeed MR60BHA2 60 GHz mmWave
kit — including on animals, which the module's own firmware will not report.

**Status: working on hardware, not yet released.** The component runs on the
device and reports to Home Assistant. Raw capture, provisioning polish and
field validation remain. See [Progress](#progress).

---

## Why this exists

The MR60BHA2 reports breathing and heart rate out of the box, but those
channels are gated behind its own target detection. That detection is built for
people. Point it at a cat and all three phase channels read exactly `0.0000` —
not a low number, a closed gate.

The way past it is a dormant *collection mode* in the stock firmware (v1.6.12)
that emits complex range-Doppler tiles **before** target detection runs. The
tiles are unconditional. Everything here is built on them.

No radar firmware is modified. Collection mode is enabled by a single command
that sets one byte in RAM, and a radar reset clears it.

## What it measures, and how well

Validated against human breath counts on a resting cat:

| recording | reported | human count | error |
|---|---|---|---|
| cat at 63 cm, 60 s | 28.9 /min | 28 | +0.9 |
| blind trial 1 | 26.5 /min | 30 | −3.5 |
| blind trial 2 | 34.6 /min | 39 | −4.4 |
| resting adult at 60 cm | 11.4 /min | — | — |
| **empty room** | **no pattern** | — | correctly rejected |

Live on the device, against a human deliberately pacing at 30 /min: **30.0 to
30.1 /min across eight consecutive updates.** That is the easy case — paced
breathing is strong and regular — but it is an end-to-end check against a known
truth, and it argues against the low-rate bias the cat trials hinted at.

**Accuracy is ±4 breaths/min.** Both blind trials read low, and the two largest
errors were on the two fastest rates, so there may be a low bias at high rates.
That is unproven at n=3 and stated rather than hidden. A time-domain estimate
is published alongside; when the two disagree, the higher has been closer.

**One band does not fit both.** The defaults are tuned for an animal. A resting
adult needs `rate_min_bpm: 6`, `rate_max_bpm: 30`, `highpass_hz: 0.05` — the
band's lower edge suppresses drift residue, and an edge low enough for a slow
human lets that residue halve a cat's reported rate. See
[docs/tuning.md](docs/tuning.md).

Reliability is decided by **window agreement**: the fraction of 30 s windows
whose spectral peak matches the session median. Across six recordings it
separates without overlap — real targets 67–100 %, an empty room 33 %. When it
fails, the sensor reports `unknown` rather than a number.

### Deliberately not attempted

**Distance.** Range localisation was tested blind and falsified — 0 to 1 hits
out of 4, with one estimator ordered *inversely* against the truth. A window
pane behind the subject appears to produce multipath that defeats it. The
breathing *rate* is unaffected, and rate is what this reports.

**Heart rate.** Tiles arrive at 4.88 sets/s, so Nyquist is 146 bpm. A cat's
heart runs 140–220 and would alias into the breathing harmonics.

## Progress

| | | |
|---|---|---|
| M0 | repo, English-only CI gate, docs | **done** |
| M1 | DSP ported to C, host replay regression | **done** — 6 of 6 recordings reproduce |
| M1b | benchmark on the ESP32-C6 | **done** — 453 ms per pass, 34 % RAM |
| M2 | ESPHome component: UART, tiles, diagnostics | **done** — 4.882 Hz on hardware, zero payload errors |
| M3 | DSP on a FreeRTOS task | **done** — 502 ms/pass live, UART unaffected |
| M4 | entities, tunable knobs, `unknown` semantics | **done** |
| M5 | raw capture over TCP | **done** — WiFi capture replays through the same harness |
| M6 | provisioning, OTA, docs | next |
| M7 | release `v0.1.0` | |
| M8 | field validation, long unattended run | |

## Entities

| entity | notes |
|---|---|
| `Breathing rate` | `/min`, **`unknown` when the verdict does not hold** |
| `Breathing detected` | the verdict as a boolean |
| `Breathing status` | `ok` / `unstable` / `low_snr` / `too_shallow` / `no_data` / `warming_up` |
| `Breathing rate (time domain)` | independent estimate; prefer the higher when they disagree |
| `Breathing stability`, `Breathing SNR`, `Movement depth` | diagnostics behind the verdict |
| `Radar sample rate` | should sit at 4.88 Hz — the UART health check |
| `Radar frame errors`, `Radar tile sets`, `Radar buffered` | link diagnostics |
| `Ambient light` | BH1750 |
| `Status LED` | WS2812, off by default |

Tuning knobs are Home Assistant number entities and persist across reboots.
See [docs/tuning.md](docs/tuning.md).

## Repository layout

```
components/mr60_breathing/dsp.{c,h}   the signal processing, plain C11
tools/replay/                         host harness: same C, recorded input
tools/bench/                          on-device benchmark (PlatformIO)
tools/rawcapture.py                   record from a deployed device
tools/check_language.py               English-only build gate
tests/fixtures/                       six recordings, compacted
tests/expected.json                   regression targets and human counts
```

`dsp.c` has **no ESPHome, Arduino or ESP-IDF dependency**. That is what lets
the identical source that runs on the microcontroller also run against the
recordings that produced the numbers above:

```sh
make test          # language gate + build + all six recordings
```

The device is checked against the host too. On the same 60 s recording the
ESP32-C6 reports 28.86 /min where the workstation reports 28.84.

## Raw capture

A deployed device can still be recorded from, over the network:

```sh
python tools/rawcapture.py breathing-monitor.local 120 capture.csv
./tools/replay/replay capture.csv
```

The wire format is not new — it is the same CSV the test fixtures use — so a
capture taken in place drops straight into the analysis that produced the
numbers above. Nothing is serialised until a client connects, so leaving it
enabled costs one non-blocking accept per loop. See
[docs/raw-capture.md](docs/raw-capture.md).

## Running it on hardware

Hardware is the Seeed MR60BHA2 kit with the XIAO ESP32-C6 (SKU p-5945).
ESPHome requires the **ESP-IDF** framework on the C6; Arduino is not available
for that variant.

Pin assignments, matching Seeed's own configuration for this kit:

| | |
|---|---|
| radar UART | GPIO17 RX, GPIO16 TX, 115200 8N1 |
| BH1750 light sensor | I²C, SDA GPIO22, SCL GPIO23, address 0x23 |
| WS2812 RGB LED | GPIO1 |

The LED and light sensor use stock ESPHome platforms and stay supported. The
LED defaults to **off**: it sits next to a sleeping animal, and a monitor that
lights up a dark room at night is a worse monitor.

## Two constraints worth knowing before you extend this

**Collection mode nearly saturates the link.** Two 1024-byte frames at
4.88 sets/s is about 10.1 kB/s against a 11.52 kB/s UART — 88 % of capacity,
leaving roughly 400 ms of buffer slack. Anything that blocks the main loop for
longer than that loses data. This is why the analysis runs on its own task, and
why a `sample_rate` diagnostic is published: degradation should be visible
rather than silent.

**The ESP32-C6 has no floating-point unit.** It is RV32IMAC, so every float
operation is a libgcc call. One analysis pass costs 453 ms, measured. The
frequency grid is the dominant term and is deliberately coarse (0.010 Hz), with
parabolic peak refinement recovering the precision — across all six recordings
that choice moves the reported rate by at most 0.02 /min while halving the cost.

## Licence

MIT. See [LICENSE](LICENSE).

The reverse engineering that made this possible builds on
[akkauppi/mr60bha2-internals](https://github.com/akkauppi/mr60bha2-internals)
for the collection-mode command frames. No vendor SDK code is included in this
repository.
