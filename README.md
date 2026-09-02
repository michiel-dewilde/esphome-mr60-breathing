# esphome-mr60-breathing

Breathing-rate sensing for Home Assistant on the Seeed MR60BHA2 60 GHz mmWave
kit — including on animals, which the module's own firmware will not report.

**v0.2.1.** Runs on hardware, reports to Home Assistant, and has been left alone
overnight. Accuracy against an animal rests on four human breath counts. Two
unattended runs have now each ended in a failure that reported nothing wrong;
v0.2.1 is the guards against both. See
[what is not proven](CHANGELOG.md#what-is-not-proven).

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

**Accuracy against an animal is not properly quantified**, and the ±4 /min this
project used to claim has been **retracted**. Three of the four counts were taken
against start and stop signals sent over a chat interface: both blind trials
imply a counting window about 7.8 seconds too long, agreeing to 0.3 s, which
accounts for the entire apparent error. The fourth, a human pacing at a known
30 /min with truth independent of that signalling, read 30.0–30.1.

What is supported: exact against a paced human, and self-consistent to 1.1 /min
across five sessions on the same cat over two mountings. See
[docs/tuning.md](docs/tuning.md) for the arithmetic and a counting protocol that
avoids the problem.

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
| M6 | provisioning, OTA, docs | **done** — 20 entities live in Home Assistant |
| M7 | release `v0.1.0` | **done** |
| M8 | field validation, long unattended run | **done** — see below |

### What field validation has shown

A cat asleep at 74 cm, measured through the deployed device over WiFi:
**29.7 /min at 14 dB with 100 % window agreement** — the same animal that
measured 28–30 /min on the bench. A human counting alongside gave 33.7 /min
measured against ~35 counted for the portion she held still.

One unattended night, 1424 overlapping windows, cat present for the first part
and an empty room after she left:

- **zero false positives** — of 824 empty windows, none reported a rate
- **66 % of occupied windows** reported one, median 27 /min

That night also exposed **27 restarts**, averaging 13 minutes of uptime. Two
separate causes, both now fixed and confirmed:

| reset reason | cause | fix |
|---|---|---|
| `software via esp_restart` | ESPHome's WiFi and API reboot timeouts, firing when the network went quiet | both set to `0s` |
| `interrupt watchdog` | WiFi **light modem sleep**, ESPHome's default, cycling the radio's PHY while the SAR arbitration holds a spinlock | `power_save_mode: none` |

Measured before and after, with a capture stream held open throughout:

| | before | after |
|---|---|---|
| spontaneous restarts | 5 in 90 minutes | **0 in 6 h 25 min** |

The second cause was only findable from the panic backtrace on the USB console.
Three theories preceded it — a stack overflow, the reboot timeouts, the capture
stream — all reasoned from *when* the restarts happened, and two were wrong.

One more thing worth knowing: **deep sleep is shallow breathing.** A sleeping cat
sits nearer an empty room in amplitude than a restless one does, which is the
opposite of the intuition and matters when setting `min_depth_um`.

### The failure mode that reports nothing wrong

A later run stopped analysing after 55 hours while the radar stayed healthy:
tiles kept arriving at 4.878 sets/s, the link diagnostics kept publishing, and
every breathing entity held the value it had last published. Home Assistant
showed a plausible status, a plausible depth and no error for 28 hours.

The cause was a clock reading that landed ahead of the analysis task's
last-run timestamp, which makes "is it due yet" false forever. Two neighbouring
ways to fail silently came out of the same investigation: `dsp_analyze` windows
on the newest sample *it holds* rather than on now, so a buffer nobody feeds
still analyses and still returns the last rate it saw; and nothing anywhere
measured whether the analysis was still producing.

Four guards close them — a monotonicity clamp on both timestamps, a staleness
refusal that publishes `no_data` and discards the buffered history, a verdict
published every interval whatever it says, and a watchdog that restarts the
device when tiles arrive but no result does. A fifth case found while fixing
them is closed the same way: a radar that keeps streaming frozen content passes
every frame check there is, so a set identical to the one before it is now
dropped before the analysis sees it and counted as a link error. The
general lesson is the one this project keeps relearning: **an entity holding
its last value looks exactly like a quiet room.** Anything that can stop
producing needs something else measuring that it still is.

## Entities

| entity | notes |
|---|---|
| `Breathing rate` | `/min`, **`unknown` when the verdict does not hold** |
| `Breathing detected` | the verdict as a boolean |
| `Breathing status` | `ok` / `unstable` / `moving` / `low_snr` / `too_shallow` / `no_data` / `warming_up`. `no_data` also covers a buffer that stopped being fed |
| `Breathing rate (time domain)` | independent estimate; prefer the higher when they disagree |
| `Breathing stability`, `Breathing SNR` | quality of the rate — neither says anything is *there* |
| `Movement depth` | amplitude, on the strongest range row. The only presence test |
| `Motion` | amplitude stationarity. Above 2.5 the subject moved and no rate is published |
| `Radar sample rate` | should sit at 4.88 Hz — the UART health check |
| `Radar frame errors`, `Radar tile sets`, `Radar buffered` | link diagnostics; frame errors include sets the radar repeated verbatim |
| `Uptime`, `Reset reason` | restart history and its cause, named |
| `Heap free`, `Heap fragmentation`, `Loop time` | firmware health |
| `Ambient light` | BH1750 |
| `Status LED` | WS2812, off by default |
| `Restart`, `Restart in safe mode` | buttons; a WiFi-managed device needs a way back |

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

## Home Assistant

Flash, power on, wait about a minute for the **Breathing Monitor Setup** access
point to appear - ESPHome scans for known networks first - then join it and the
captive portal asks for your network. Home Assistant's ESPHome integration
discovers it from there; later updates go over the air.

Twenty entities arrive, of which three matter: `Breathing rate` (the
measurement), `Breathing detected` (the verdict) and `Breathing status` (why,
when there is no rate). Dashboard cards, automations and the tuning knobs are
in [docs/home-assistant.md](docs/home-assistant.md).

**This is not a safety device.** It has never been validated as one, and its
range window of 45.9-86.1 cm is where it works properly rather than a wall: a
subject beyond the far edge can still be detected, with its energy piling into
the last range row, or can be invisible - depending on geometry, and with no
way to tell which from the reading.

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

## Changelog

[CHANGELOG.md](CHANGELOG.md), which also lists what is *not* proven.

## Licence

MIT. See [LICENSE](LICENSE).

The reverse engineering that made this possible builds on
[akkauppi/mr60bha2-internals](https://github.com/akkauppi/mr60bha2-internals)
for the collection-mode command frames. No vendor SDK code is included in this
repository.
