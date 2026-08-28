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
| `too_shallow` | below the depth gate | nothing there, or the subject is beyond 86 cm |
| `moving` | the subject shifted during the window | wait for it to settle; nothing to fix |

## `unstable` is usually correct

Before loosening the threshold, consider that the sensor may simply be right:
the animal moved, or left.

**Stability is a much weaker presence test than it first appeared.** An early
reading of six recordings suggested empty rooms scored 33 % against 67–100 % for
real targets. Measured properly — 824 windows from a room the cat had left —
**66 % of them still pass a 60 % stability gate**, with a median of 66.7 %. An
empty room holds a steady spectral peak far more often than those six recordings
implied. Stability tells you the *rate* is trustworthy; it barely tells you
anything is *there*. Depth does that.

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

Depth is measured on the **strongest range row** — the bin that sees the target
best — not on some average. That distinction was learned the hard way: it used
to be read from the row the coherent average uses as its reference, which is
chosen for phase coherence rather than strength. On a clean recording of a cat
asleep at 74 cm, that reference row held 2.9 µm while the row that actually saw
her held 59.7, and a perfectly good measurement was thrown away as too shallow.

Measured references, in the cat band:

| scene | depth |
|---|---|
| empty room | 7.7 µm |
| cat at 1 m — beyond the window | 26 µm |
| cat at 76 cm | 46 µm |
| **cat asleep at 74 cm** | **55 µm** |
| cat at 46 cm | 374 µm |
| cat at 63 cm | 562 µm |

The 20 µm default sits better than a factor of two either side of the gap
between an empty room and the weakest animal. A person reaches hundreds of µm,
so the human profile wants a higher floor.

Note the sleeping cat: deep sleep is *shallower* breathing, and it lands nearer
the empty room than a restless animal does. If you raise this threshold to
silence a noisy room, you may silence a sleeping animal too.

## When the subject moves

A body that shifts displaces far more than a chest that breathes, and it does
not announce itself — it produces a confident wrong answer. Measured on a cat
that settled and then moved: 18 seconds of movement inside a 60 s window pulled
the reported rate from 33.7 to 19.7 /min, with the verdict still reading `ok`.

The **Motion** diagnostic catches it. It is not an amplitude but a
*stationarity*: the loudest 15 s sub-window divided by the median one. Averaged
over a minute, movement is indistinguishable from a large calm animal — a
settled cat reached 59 µm and one that moved 66 — but as non-stationarity they
separate cleanly:

| scene | motion |
|---|---|
| cat asleep | 1.04 |
| settled cats | 1.27 – 1.70 |
| **cat that moved** | **4.37** |

Above `max_motion_ratio` (2.5 by default) the status becomes `moving` and no
rate is published. That is the honest answer: the analysis assumes one rate
holds across the window, and movement breaks the assumption.

Treat the figure as a **lower bound**, not a calibrated displacement. It comes
from the strongest range row, and clutter in the same bin dilutes it.

## Do the settings survive a reboot?

Yes. Every knob is stored on the device and restored at boot — verified by
setting one, rebooting the device with a firmware update, and reading it back
unchanged. They are the device's settings, not Home Assistant's, so they also
survive Home Assistant restarting or going away entirely.

They are also *applied* at boot, not merely displayed. That distinction cost
this project a real bug: ESPHome publishes a number's restored value without
running its `set_action`, so for a while the device came back from a reboot
showing a 10 um depth gate in Home Assistant while analysing with the gate off,
and duly reported a breathing rate for an empty room. The configuration now
pushes every restored value into the component explicitly at boot. If you add a
knob of your own, add it there too.

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

**Short intervals cost network, not just CPU**, and the price is steeper than
it looks. Measured on a deployed device set to a 1 s interval: ping averaged
84 ms and the raw capture stream dropped every two or three minutes, because a
pass that takes half a second leaves the loop that services the network almost
no time. Returning it to 10 s took ping to 25 ms and the drops stopped
outright.

The 10 s default is a 4.5 % duty cycle. There is little reason to go below a
couple of seconds: the analysis window is 60 s, so consecutive results share
almost all of their input and barely differ. A shorter interval buys you a
number that changes more often, not a number that is fresher.

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

## If the device keeps restarting

Watch **Uptime** and **Reset reason**. Between them they distinguish every cause
worth distinguishing, and each has a different fix:

| reset reason | meaning | what to do |
|---|---|---|
| `Reboot request from esphome.ota` | you installed firmware | nothing |
| `USB peripheral` | a serial connection reset it | nothing; opening the port resets this board |
| `software via esp_restart` | the firmware restarted itself deliberately | usually the WiFi or API reboot timeout; both ship disabled here |
| `interrupt watchdog` | interrupts were blocked past 300 ms | see below |
| `task watchdog` | a task hogged the CPU | check `Loop time` |
| `brownout` | the supply sagged | a power problem, not a software one |

`interrupt watchdog` on this board came from **WiFi light modem sleep**, which is
ESPHome's default. The radio's PHY is powered down and up continuously and the
arbitration around it holds a spinlock; hold it too long and the chip resets.
The shipped configuration sets `power_save_mode: none` for that reason. If you
copy pieces of this configuration elsewhere, take that line with you.

Measured either side of that change, with a capture stream held open throughout:
**5 restarts in 90 minutes before, none in 6 hours 25 minutes after.**

Diagnosing it needed the panic backtrace, which only appears on the USB console.
Everything before that — three plausible theories about stacks, timeouts and the
capture stream — was inference from timing, and two of the three were wrong.

### Beyond 86 cm: less absolute than it sounds

The documentation used to say the sensor reports nothing outside 45.9-86.1 cm.
That is too strong. A target past the far edge does not vanish - its return
piles into the last range row, because there is nowhere further for it to go.
Measured with a cat about 1.2 m away and slightly off to one side:

| range row | 45.9 | 51.7 | 57.4 | 63.1 | 68.9 | 74.6 | 80.4 | 86.1 |
|---|---|---|---|---|---|---|---|---|
| cat beyond the window | 66 | 64 | 48 | 8 | 25 | 35 | 65 | **170** |
| the same spot, empty | 4.5 | 4.0 | 5.2 | 0.6 | 1.9 | 2.8 | 5.5 | 8.8 |

It reported 28.3, 30.9 and 30.4 /min in that state, against a true rate of about
29-30. So detection well past the window is possible, and the rate can even be
right.

Do not rely on it, and note which way round the two measurements fall. The same
animal at **1 m**, in a different position, produced a range profile flatter
than an empty room and was invisible - while at **1.2 m** here she was
unmistakable. The nearer target was the undetectable one, so distance alone does
not decide it. Geometry and whatever else is reflecting do, and the reading does
not tell you which case you are in: a monotonic rise towards the last row is a
hint, not a diagnosis.

The practical consequence for a nest or bed: **something lying beside it will
be seen much like something lying in it.** Position for the subject you mean to
watch, and treat presence as "something is breathing in the beam" rather than
"the subject is where I put the sensor".

## What this cannot do

**Distance.** Range localisation was tested blind and failed — 0 to 1 hits out
of 4, with one estimator ordered inversely against the truth. A window pane
behind the subject appears to cause it. The rate is unaffected; the
`range_row_min` / `range_row_max` options gate which rows contribute, but do
not tell you where anything is.

**Heart rate.** Tiles arrive at 4.88 sets/s, so Nyquist is 146 bpm. A cat's
heart runs 140–220 and aliases into the breathing harmonics.
