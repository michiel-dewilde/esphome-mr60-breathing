# Changelog

## v0.2.0 — 28 August 2026

Everything here came out of running v0.1.0 on a real installation — an
afternoon, a night alone, and a morning of chasing restarts. None of it was
visible on the bench.

v0.1.0 said the device had never run unattended and that this was the actual
product. It has now: six and a half hours without a restart, and a night that
never once invented a breathing rate for an empty room. What it has still not
had is a *clean* unattended night — the one it ran was underneath a device
restarting every thirteen minutes.

### What it now measures

A cat asleep at 74 cm, through the deployed device over WiFi: **29.7 /min at
14 dB with 100 % window agreement** — the same animal the bench measured at
28–30. Counted alongside by hand: 33.7 /min measured against ~35 counted, for
the portion she held still.

One unattended night, 1424 overlapping windows, cat present then an empty room:

- **zero false positives** — of 824 empty windows, none reported a rate
- **66 % of occupied windows** reported one, median 27 /min

### Four defects only real use exposed

- **An empty room was reported as breathing, confidently.** 57 /min, 100 %
  stability, 13.4 dB — a higher SNR than a real cat. Stability and SNR are both
  scale-invariant by construction, so a coherent disturbance of any amplitude
  passes them. `min_depth_um` now ships enabled at 20 µm.
- **Depth was read from the wrong row.** The reference row is chosen for phase
  coherence, not signal strength, and on a sleeping cat it held 2.9 µm while the
  row that saw her held 59.7 — throwing away a good measurement as too shallow.
  Depth is now taken from the strongest row.
- **Movement was invisible.** Eighteen seconds of movement inside a 60 s window
  pulled the reported rate from 33.7 to 19.7 /min with the verdict still reading
  `ok`. A level threshold cannot catch it — averaged over a minute, movement
  looks like a large calm animal. Stationarity can: settled subjects score
  1.04–1.70, a cat that shifted scored 4.37. New `moving` status.
- **Restored settings were displayed but never applied.** ESPHome publishes a
  number's restored value at boot without running its `set_action`, so after
  every reboot the device analysed with compile-time defaults while Home
  Assistant showed the stored ones. `on_boot` now pushes them in explicitly.

### The restarts

27 in one night, averaging 13 minutes of uptime. Two causes:

| reset reason | cause | fix |
|---|---|---|
| `software via esp_restart` | WiFi and API reboot timeouts firing when the network went quiet | both `0s` |
| `interrupt watchdog` | WiFi light modem sleep cycling the PHY while the SAR arbitration holds a spinlock | `power_save_mode: none` |

Measured with a capture stream held open throughout: **5 restarts in 90 minutes
before, 0 in 6 h 25 min after.**

The second was only findable from the panic backtrace on the USB console. Three
theories preceded it — a stack overflow, the reboot timeouts, the capture stream
— all reasoned from when the restarts happened, and two were wrong. The stack
change was kept anyway: it reduced a real risk on a 3584-byte main task, and the
stack now sits at 8192.

### Recalibrated on real data

1424 windows from that night, half occupied and half empty:

- **`min_snr_db` 6 → 4.** At 6 it rejected a quarter of the windows in which the
  cat was demonstrably present while changing no verdict on any empty one.
- **`min_depth_um` left at 20**, not raised into the middle of the measured gap.
  One night samples one animal in a few positions, and the weakest in-window
  reading on record is 45.8 µm.

### Corrections to earlier claims

- Empty rooms do **not** score 33 % stability against 67–100 % for real targets.
  That came from six recordings. Across 824 empty windows, 66 % pass a 60 %
  gate. Stability says the rate is trustworthy, not that anything is there.
- Range concentration is **not** a presence indicator. It runs higher in an
  empty room than with a cat present — the reverse of what a few fixtures
  suggested. Retracted before it reached the documentation.

### Added

Restart and safe-mode buttons; `Motion`, `Uptime`, `Reset reason`, `Heap free`,
`Heap fragmentation` and `Loop time` diagnostics; a reset-reason table in the
tuning guide; and a note that ESPHome caches remote packages for a day, which
lets an Install finish in seconds, report success, flash, and contain none of
your changes.

Three of the defects above are the same mistake in different clothes: trusting
the value written instead of reading back the value that took effect.

## v0.1.0 — 27 August 2026

First release. It works on hardware and reports to Home Assistant, but it has
not yet been left running unattended, and its accuracy against an animal rests
on three human breath counts. Read [What is not proven](#what-is-not-proven)
before relying on it for anything.

### What it does

Reports breathing rate from a Seeed MR60BHA2 60 GHz mmWave kit, **including for
animals**, whose breathing the module's own firmware refuses to report because
its target detection never fires for them. The way past that gate is a dormant
collection mode in the stock firmware that emits raw range-Doppler tiles before
detection runs.

No radar firmware is modified. Collection mode sets one byte in RAM and a radar
reset clears it.

### Accuracy

| subject | reported | truth | error |
|---|---|---|---|
| paced human, live on the device | 30.0–30.1 /min | 30 (paced) | ~0.0 |
| cat at 63 cm, 60 s | 28.9 /min | 28 (counted) | +0.9 |
| blind trial 1 | 26.5 /min | 30 (counted) | −3.5 |
| blind trial 2 | 34.6 /min | 39 (counted) | −4.4 |
| resting adult, recorded | 11.4 /min | — | — |
| **empty room** | **no pattern** | — | correctly rejected |

**±4 breaths/min against an animal.** Against a human deliberately pacing at a
known rate it was essentially exact, but that is the easy case: paced breathing
is strong, regular and nearly sinusoidal.

Reliability is decided by window agreement — the fraction of 30 s windows whose
spectral peak matches the session median. Across six recordings it separates
without overlap: real targets 67–100 %, an empty room 33 %. When it fails, the
rate is published as `unknown` rather than as a number.

### Components

- `mr60_breathing` — ESPHome external component, ESP-IDF, ESP32-C6
- 20 entities: rate, verdict, status, and diagnostics for both the analysis and
  the radar link
- Tuning knobs as Home Assistant number entities, persistent across reboots
- Raw capture over TCP, in a format the existing tooling already parses
- BH1750 ambient light and the WS2812 LED, via stock ESPHome platforms
- WiFi access point and captive portal for provisioning; OTA thereafter

### Engineering notes worth knowing

**The ESP32-C6 has no floating-point unit.** One analysis pass costs about
450 ms measured, 500 ms live. It runs on its own FreeRTOS task with cooperative
yields — priority alone cannot solve it, since below the idle task it starves
and above the main loop it blocks the UART.

**Collection mode nearly saturates the radar link** — about 10.1 kB/s against
11.52 kB/s, 88 % of capacity, with roughly 400 ms of buffer slack. The
`Radar sample rate` diagnostic exists so that degradation is visible rather than
silent. It should read 4.88 Hz.

**The band's lower edge is a drift guard, not just a search range.** One band
cannot serve both a cat and a resting adult: an edge low enough for a slow human
admits drift residue that outweighs a cat's breathing peak and halves the
reported rate. Defaults are the animal profile; see `docs/tuning.md`.

**The identical DSP source runs on the device and on a workstation.** `dsp.c`
has no ESPHome, Arduino or ESP-IDF dependency, so `make test` replays it against
the six recordings that produced the numbers above. On the same input the device
reports 28.86 /min where the host reports 28.84.

### What is not proven

- **Never run unattended.** Every measurement so far is 60–180 s with a person
  in the room. This is the actual product and it is untested. *(Superseded: see
  the unreleased changes above.)*
- **Not a safety device.** Do not use it where a missed reading matters.
- **Distance is not attempted.** Range localisation was tested blind and
  falsified — 0 to 1 hits out of 4, one estimator ordered inversely against the
  truth. A window pane behind the subject appears to cause it.
- **Heart rate is not attempted.** Tiles arrive at 4.88 sets/s, so Nyquist is
  146 bpm; a cat's heart runs 140–220 and aliases into the breathing harmonics.
- **`min_depth_um` ships disabled.** An honest floor has to be measured on the
  installation rather than guessed. *(Superseded: it ships at 20 µm as of the
  unreleased changes above.)*
- **Range is fixed at 45.9–86.1 cm**, set by the eight range rows collection
  mode sends. Outside it, the sensor reports nothing.
- The spectral estimate read low on both blind cat trials. The paced-human test
  argues against a systematic bias, but n=3 against a cat is not much.
