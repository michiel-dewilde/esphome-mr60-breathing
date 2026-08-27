# Tuning

The defaults are tuned for an **animal** at 45–85 cm. A person needs a
different band — see [Choosing a band](#choosing-a-band-animal-or-person-not-both).
Everything below is a Home Assistant number entity, persists across reboots,
and can be changed while the device runs.

Before changing anything, read the diagnostics. They usually say what is wrong.

## Reading the diagnostics

| entity | healthy | what it means when it is not |
|---|---|---|
| **Radar sample rate** | 4.88 Hz | Below that, tile sets are being lost. Collection mode uses about 88 % of the UART's capacity, so anything that stalls the main loop shows up here first. |
| **Radar frame errors** | flat | Rising steadily means checksum failures or unpaired tiles. A few at startup are normal — the parser resynchronises byte by byte. |
| **Radar buffered** | ≈ your window | How much history the analysis has. It fills over the first minute after boot. |
| **Breathing stability** | ≥ 60 % | The fraction of 30 s windows agreeing on a rate. This is the reliability test. |
| **Breathing SNR** | ≥ 6 dB | Peak height above the band floor. |
| **Movement depth** | see below | Chest displacement, **a lower bound**. |

## The status text sensor tells you which knob to reach for

| status | meaning | what to do |
|---|---|---|
| `ok` | rate is being published | — |
| `warming_up` | not enough history yet | wait a minute; if it persists, check the sample rate |
| `no_data` | no tiles, or every row flat | check wiring and the sample rate |
| `unstable` | the peak moves between windows | see below — this is the interesting one |
| `low_snr` | a peak, but weak | move the sensor closer, or lower `Minimum SNR` knowing what that costs |
| `too_shallow` | below the depth gate | your `Minimum depth` is set too high for this subject |

## `unstable` is usually correct

The spectral peak moving between windows is what an empty room does — measured
at 33 %, against 67–100 % for a real target. Before loosening the threshold,
consider that the sensor may simply be right: the animal moved, or left.

If it is wrong, the likely causes in order:

1. **The subject is outside 45.9–86.1 cm.** That range is fixed by the eight
   range rows the radar sends in collection mode. Outside it, there is nothing
   to measure. Move the sensor, not the knob.
2. **The band does not match the subject.** The defaults are the animal
   profile; a resting adult below 12 /min falls off its lower edge. See the
   next section — this is the single most common misconfiguration.
3. **Something else in range is moving.** A fan, a curtain, a second animal.

## Choosing a band: animal or person, not both

**The defaults are the animal profile.** For a person, set:

| knob | animal (default) | person |
|---|---|---|
| `rate_min_bpm` | 12 | **6** |
| `rate_max_bpm` | 66 | **30** |
| `highpass_hz` | 0.10 | **0.05** |

This is not a convenience. The band's lower edge is a **drift guard**, not just
a search range. Unwrapped radar phase wanders, and the residue of that wander
piles up just above the high-pass corner. Measured on the reference
recordings, dropping the edge from 12 to 8 /min lets the residue outweigh the
breathing peak and the cat rates halve:

| band minimum | cat 63 cm (truth 28) | blind 1 (truth 30) | adult (truth 11.4) |
|---|---|---|---|
| 12 /min | **28.9** | **26.5** | 12.0 — clipped at the edge |
| 8 /min | 14.5 | 12.4 | 11.5 |

The halved figures are not a sub-harmonic guard misfiring — forcing that guard
off changes nothing. At the wider band 14.5 /min genuinely *is* the largest
peak. The narrow band is what keeps it out.

So a resting adult below 12 /min needs the person profile, and a cat needs the
animal one. Running the animal defaults on a slow-breathing person reads high
and sits on the band edge; running the person profile on a cat can halve it.

## The wide default band is the least safe setting

The 12-66 /min default exists so the device does something sensible before you
have configured anything. It is not the best setting for any actual subject,
and on one real installation it manufactured a breathing rate out of a quiet
room: a persistent interference near 57 /min sat inside the band and was
reported confidently.

Narrowing the band to the subject removed it outright, on the same recording:

| band | empty room | real cats (28 / 30 / 39 counted) |
|---|---|---|
| 12-66 /min | **57.5 /min, 100 % stable, 8.3 dB** | 28.9, 26.5, 34.6 — all ok |
| 18-45 /min | 20.9 /min, 33 % stable, 2.1 dB — rejected | 28.9, 26.5, 34.6 — all ok |

Nothing real was lost and the false positive disappeared on every criterion at
once. **If you know what you are watching, narrow the band.** A resting cat sits
near 30 /min, so 18-45 is generous; an adult sits near 12, so 6-30 with
`highpass_hz: 0.05` suits them. The wide default only makes sense while you do
not yet know.

Depth alone will not save you here. On that same empty room it wandered between
2 and 28 um over a few minutes, crossing the 10 um gate in both directions,
while the band change settled it permanently.

## Setting `Minimum depth`

This gate ships at **10 µm**, and it is the only defence against a whole class
of false positive.

**Stability and SNR are both scale-invariant by construction.** They are
computed on the per-row normalised, coherently averaged signal, so a coherent
disturbance of *any* amplitude passes them. Measured on a live empty room, a
2 µm interference at 57 /min scored 100 % stability and 13.4 dB — a higher SNR
than a real cat at 76 cm — and was published as a confident breathing rate.
Depth is the only number in the result that carries absolute amplitude.

To tune it for your installation: watch **Movement depth** with the subject
present and settled, then again with the room empty. Put the threshold between
the two, nearer the empty value. Measured references:

| scene | depth |
|---|---|
| empty room, 5 minutes steady | 2–5 µm |
| cat at 46 cm | 21 µm |
| cat at 76 cm | 26 µm |
| cat at 63 cm | 40 µm |
| adult at 60 cm | 3600 µm |

The 10 µm default sits about twice the empty-room reading and about half the
weakest animal measured. An adult is two orders of magnitude above a cat, which
is why one threshold cannot be ideal for both.

Brief motion — someone walking past — produces tens of µm and will pass the
gate. That is not a failure of the gate: motion genuinely displaces things. The
stability test is what rejects it, because a passing disturbance does not hold
one frequency across windows.

Treat the figure as a **lower bound**, not a calibrated displacement. It comes
from the strongest range row, and clutter in the same bin dilutes it.

## Do the settings survive a reboot?

Yes. Every knob is stored on the device and restored at boot — verified by
setting one, rebooting the device with a firmware update, and reading it back
unchanged. They are the device's settings, not Home Assistant's, so they also
survive Home Assistant restarting or going away entirely.

Two things can still lose a setting:

- **A change made in the last minute before power is cut.** ESPHome batches
  writes to flash (about once a minute by default) rather than writing on every
  change, to spare the flash. Pull the plug straight after moving a slider and
  the change may not have been written yet.
- **A change to the entity in the configuration.** Stored values are keyed to
  the entity's identity. Rename it, or change its ID, and the old value is
  orphaned and the entity comes back at its configured `initial_value`.

The second has a consequence worth knowing: **a new default in a firmware
update does not reach a device that already has a stored value.** The stored
one wins, which is what you want for settings you have chosen deliberately, and
not what you want when a default changed because it was wrong. After an update
that changes a default, set it once by hand.

## Update interval and what `0` really means

`0` means "start the next analysis as soon as the last one finished". On this
chip that is roughly **2 Hz, not the 4.88 Hz tile rate**: one pass costs about
450 ms because the ESP32-C6 has no floating-point unit.

Running continuously keeps a core busy and will show up as WiFi latency. The
10 s default is a 4.5 % duty cycle. There is little reason to go below a couple
of seconds — the analysis window is 60 s, so consecutive results share almost
all their input and barely differ.

## Accuracy, honestly

Against a **paced** human breathing at a known 30 /min, the device read 30.0 to
30.1 across eight consecutive updates — essentially exact. That is the easy
case: deliberate breathing is strong, regular and nearly sinusoidal.

Against a **cat**, the reported rate is accurate to about **±4 /min**, measured
against three human counts. Both blind trials read *low*, and the two largest errors were on
the two fastest rates, which suggested a low bias at high rates. The paced test
above argues against that: a known 30 /min read 30.0, where a biased estimator
should have read about 27. The residual error is more likely in how hard a
cat's shallow breathing is to count by eye than in the estimator.

**Breathing rate (time domain)** is a second, independent estimate from
peak-to-peak intervals. When the two disagree by more than a couple of breaths,
the higher one has been closer to the truth.

## What this cannot do

**Distance.** Range localisation was tested blind and failed — 0 to 1 hits out
of 4, with one estimator ordered inversely against the truth. A window pane
behind the subject appears to cause it. The rate is unaffected; the
`range_row_min` / `range_row_max` options gate which rows contribute, but do
not tell you where anything is.

**Heart rate.** Tiles arrive at 4.88 sets/s, so Nyquist is 146 bpm. A cat's
heart runs 140–220 and aliases into the breathing harmonics.
