# Home Assistant

## Getting the device onto your network

Flash `breathing-monitor.yaml`, then power the board.

**Wait about a minute.** ESPHome scans for known networks before falling back to
its own access point, so **Breathing Monitor Setup** does not appear
immediately - and the clock restarts on every reset. It is easy to conclude the
access point is broken when it has simply not been given its minute yet.

Once it appears, connect to it and a captive portal asks for your WiFi
network.

After that the device is discovered by Home Assistant's ESPHome integration in
the usual way — **Settings → Devices & Services** will offer it. Later updates
go over the air; the USB cable is only needed for the first flash.

## Adopting it, and the package cache

Adopting the device writes a small wrapper configuration in your ESPHome
dashboard that pulls this repository as a package:

```yaml
packages:
  michiel_dewilde.mr60_breathing: github://michiel-dewilde/esphome-mr60-breathing/breathing-monitor.yaml@main
```

**ESPHome caches that package for a day.** Until the cache expires, pressing
Install rebuilds against the copy it already has — the build finishes in
seconds, reports success, flashes, reboots, and contains none of your updates.
Nothing warns you. The tell is the speed, and the absence of whatever you were
expecting.

While this component is changing, expand the shorthand so the package is
re-fetched every time:

```yaml
packages:
  michiel_dewilde.mr60_breathing:
    url: https://github.com/michiel-dewilde/esphome-mr60-breathing
    ref: main
    files: [breathing-monitor.yaml]
    refresh: 0s
```

`refresh: 0s` costs a git fetch per build and removes an entire class of
confusion. Raise it once you are pinning to releases rather than following the
branch.

Note that "Clean Build Files" does not help here: it clears compiled objects,
not the package cache.

## The entities, and which ones matter

| entity | what to do with it |
|---|---|
| **Breathing rate** | the measurement. `unknown` whenever the verdict does not hold |
| **Breathing detected** | the verdict as a boolean — the right trigger for automations |
| **Breathing status** | why, when the rate is missing: `ok`, `unstable`, `moving`, `low_snr`, `too_shallow`, `no_data`, `warming_up` |
| Breathing rate (time domain) | independent estimate; when the two disagree, the higher has been closer |
| Breathing stability, Breathing SNR | how trustworthy the rate is — neither says anything is *present* |
| Movement depth | amplitude on the strongest range row; the presence test |
| Motion | amplitude stationarity; above 2.5 the subject moved and no rate is published |
| Radar sample rate | should sit at 4.88 Hz — the health check that matters |
| Radar frame errors, tile sets, buffered | link diagnostics |
| Uptime, Reset reason | restart history, and the cause of the last one by name |
| Heap free, Heap fragmentation, Loop time | firmware health |
| Ambient light | BH1750 on the carrier board |
| Status LED | WS2812, off by default |
| Restart, Restart in safe mode | buttons |

`Breathing rate` reports **`unknown`**, not `0`, when it cannot measure
reliably. This is deliberate: `0` is a plausible-looking number that would land
in your history graphs and long-term statistics, and no automation could tell
it apart from a real reading.

That has a consequence worth designing around: **do not trigger on the rate
being low.** Trigger on `Breathing detected` going off, or on `Breathing
status`, both of which say what they mean.

## A dashboard card

```yaml
type: entities
title: Breathing
entities:
  - entity: sensor.breathing_monitor_breathing_rate
  - entity: binary_sensor.breathing_monitor_breathing_detected
  - entity: sensor.breathing_monitor_breathing_status
  - type: section
    label: Signal
  - entity: sensor.breathing_monitor_breathing_stability
  - entity: sensor.breathing_monitor_breathing_snr
  - entity: sensor.breathing_monitor_movement_depth
  - type: section
    label: Link
  - entity: sensor.breathing_monitor_radar_sample_rate
```

Entity IDs depend on the device name you chose; check
**Developer tools → States**.

## Automations worth having

### Warn when the radar link degrades

The single most useful alert, because it fails quietly. Collection mode uses
about 88 % of the radar's UART capacity, so a sample rate below 4.8 Hz means
tile sets are being lost and every measurement above is built on less data than
it should be.

```yaml
automation:
  - alias: Breathing monitor link degraded
    triggers:
      - trigger: numeric_state
        entity_id: sensor.breathing_monitor_radar_sample_rate
        below: 4.8
        for: "00:05:00"
    actions:
      - action: notify.persistent_notification
        data:
          message: >-
            Radar sample rate has dropped to
            {{ states('sensor.breathing_monitor_radar_sample_rate') }} Hz.
            Tile sets are being lost.
```

### Notice when a settled subject stops being measurable

Note the generous `for:` — a cat shifting position will drop the verdict for a
minute at a time, and that is the sensor working correctly, not an event.

```yaml
automation:
  - alias: Breathing no longer detected
    triggers:
      - trigger: state
        entity_id: binary_sensor.breathing_monitor_breathing_detected
        from: "on"
        to: "off"
        for: "00:10:00"
    actions:
      - action: notify.persistent_notification
        data:
          message: >-
            No stable breathing for ten minutes. Status is
            {{ states('sensor.breathing_monitor_breathing_status') }}.
```

**This is not a safety device.** It has never been validated for unattended
monitoring, its accuracy against an animal is about ±4 breaths/min, and it
reports nothing at all whenever the subject is outside 45.9–86.1 cm. Treat any
alert as a prompt to go and look.

### Use the status LED as a local indicator

Off by default, deliberately — it sits next to a sleeping animal. If you want
it to mean something:

```yaml
automation:
  - alias: Status LED follows the verdict
    triggers:
      - trigger: state
        entity_id: sensor.breathing_monitor_breathing_status
    actions:
      - choose:
          - conditions:
              - condition: state
                entity_id: sensor.breathing_monitor_breathing_status
                state: "ok"
            sequence:
              - action: light.turn_on
                target: {entity_id: light.breathing_monitor_status_led}
                data: {rgb_color: [0, 60, 0], brightness: 40}
        default:
          - action: light.turn_off
            target: {entity_id: light.breathing_monitor_status_led}
```

## Tuning from Home Assistant

The knobs are number entities under the device's **Configuration** section and
persist across reboots. The one most likely to need changing is the band: the
defaults suit an animal, and a resting adult needs a lower one. See
[tuning.md](tuning.md).

## Recording data for later analysis

The **Raw capture** switch controls a TCP listener on the device. Nothing is
serialised until something connects, so leaving it on costs nothing.

```yaml
shell_command:
  capture_breathing: >-
    python3 /config/scripts/rawcapture.py breathing-monitor.local 120
    /config/captures/{{ now().strftime('%Y%m%d_%H%M%S') }}.csv
```

See [raw-capture.md](raw-capture.md).

## Watching for restarts

**Uptime** is the entity to graph. A straight climb is a healthy device; a
sawtooth is one restarting. **Reset reason** names the cause of the most recent
one, and the causes are genuinely distinguishable — an install reads
`Reboot request from esphome.ota`, a serial connection reads `USB peripheral`,
and a fault reads `interrupt watchdog` or `task watchdog`. The tuning guide has
a table mapping each to its fix.

This configuration disables the two restarts ESPHome performs by itself: the
WiFi and API reboot timeouts, which fire when the network goes quiet. For a
device whose job is to keep watching something, restarting because Home
Assistant became unreachable is wrong twice over — it discards the analysis
history, and it stops the measurement exactly when nobody is looking.

## API encryption

`breathing-monitor.yaml` ships with a plain `api:` block so a first flash works
without a `secrets.yaml`. For anything permanent, add a key:

```yaml
api:
  encryption:
    key: !secret api_encryption_key
```

Generate one at <https://esphome.io/components/api.html> and put it in
`secrets.yaml` next to the configuration.

If you later flash over USB from a workstation, remember that the published
configuration has no key: flashing it directly strips the one the device is
using and Home Assistant will stop connecting until you reinstall. The way
around it is a small local wrapper, kept out of version control, that includes
the published configuration and adds only the key:

```yaml
# local-flash.yaml, gitignored
packages:
  base: !include breathing-monitor.yaml
api:
  encryption:
    key: !secret api_encryption_key
```
