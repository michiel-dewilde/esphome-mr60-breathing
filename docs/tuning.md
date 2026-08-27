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

## Setting `Minimum depth`

This gate ships **disabled** (0), because an honest value has to be measured on
your installation rather than guessed.

To set it: watch **Movement depth** with the subject present and settled, then
again with the room empty. Put the threshold between the two, nearer the empty
value. For reference, on the recordings behind this project a cat at 63 cm read
about 40 µm and an adult at 60 cm about 3600 µm — two orders of magnitude
apart, which is why one number cannot serve both.

Treat the figure as a **lower bound**, not a calibrated displacement. It comes
from the strongest range row, and clutter in the same bin dilutes it.

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
