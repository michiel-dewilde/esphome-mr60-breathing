# Home Assistant

## Getting the device onto your network

Flash `breathing-monitor.yaml`, then power the board. It brings up an access
point called **Breathing Monitor Setup**; connect to it and a captive portal
asks for your WiFi network.

After that the device is discovered by Home Assistant's ESPHome integration in
the usual way — **Settings → Devices & Services** will offer it. Later updates
go over the air; the USB cable is only needed for the first flash.

## The entities, and which ones matter

| entity | what to do with it |
|---|---|
| **Breathing rate** | the measurement. `unknown` whenever the verdict does not hold |
| **Breathing detected** | the verdict as a boolean — the right trigger for automations |
| **Breathing status** | why, when the rate is missing: `ok`, `unstable`, `low_snr`, `too_shallow`, `no_data`, `warming_up` |
| Breathing rate (time domain) | independent estimate; when the two disagree, the higher has been closer |
| Breathing stability, SNR, Movement depth | the three numbers behind the verdict |
| Radar sample rate | should sit at 4.88 Hz — the health check that matters |
| Radar frame errors, tile sets, buffered | link diagnostics |
| Ambient light | BH1750 on the carrier board |
| Status LED | WS2812, off by default |

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
