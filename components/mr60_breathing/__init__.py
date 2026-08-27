"""Breathing-rate sensing from MR60BHA2 collection-mode range-Doppler tiles.

The module's own breathing output is gated behind its target detection, which
never fires for an animal. This component reads the raw tiles instead, which
are emitted before that gate, and does the estimation itself.

The signal processing lives in dsp.c and is deliberately free of any ESPHome
dependency, so the identical source is regression-tested on a workstation
against the recordings that produced the published accuracy figures.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = ["@michiel-dewilde"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]

mr60_breathing_ns = cg.esphome_ns.namespace("mr60_breathing")
MR60Breathing = mr60_breathing_ns.class_(
    "MR60Breathing", cg.Component, uart.UARTDevice
)

CONF_MR60_BREATHING_ID = "mr60_breathing_id"

CONF_WINDOW_SECONDS = "window_seconds"
CONF_RATE_MIN_BPM = "rate_min_bpm"
CONF_RATE_MAX_BPM = "rate_max_bpm"
CONF_HIGHPASS_HZ = "highpass_hz"
CONF_STABILITY_WINDOW_SECONDS = "stability_window_seconds"
CONF_STABILITY_TOLERANCE_BPM = "stability_tolerance_bpm"
CONF_STABILITY_THRESHOLD_PCT = "stability_threshold_pct"
CONF_MIN_SNR_DB = "min_snr_db"
CONF_MIN_DEPTH_UM = "min_depth_um"
CONF_RANGE_ROW_MIN = "range_row_min"
CONF_RANGE_ROW_MAX = "range_row_max"
CONF_UPDATE_SECONDS = "update_seconds"


def _validate(config):
    if config[CONF_RATE_MAX_BPM] <= config[CONF_RATE_MIN_BPM] + 3:
        raise cv.Invalid(
            f"{CONF_RATE_MAX_BPM} must be at least 3 /min above "
            f"{CONF_RATE_MIN_BPM}; the search band would otherwise be too "
            "narrow to hold a peak"
        )
    if config[CONF_RANGE_ROW_MAX] < config[CONF_RANGE_ROW_MIN]:
        raise cv.Invalid(f"{CONF_RANGE_ROW_MAX} must not be below "
                         f"{CONF_RANGE_ROW_MIN}")
    # Three windows is the minimum at which the agreement statistic means
    # anything; see the MIN_STABILITY_WINDOWS note in dsp.c.
    if config[CONF_WINDOW_SECONDS] < 2 * config[CONF_STABILITY_WINDOW_SECONDS]:
        raise cv.Invalid(
            f"{CONF_WINDOW_SECONDS} must be at least twice "
            f"{CONF_STABILITY_WINDOW_SECONDS}, otherwise fewer than three "
            "stability windows form and no verdict can be given"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MR60Breathing),
            # The analysis window. Capped near 100 s by DSP_MAX_SAMPLES; going
            # higher needs that raised, and it costs static RAM.
            cv.Optional(CONF_WINDOW_SECONDS, default=60): cv.int_range(30, 100),
            # Band edges in breaths per minute rather than Hz: this watches
            # animals and people, and nobody thinks about breathing in Hz. The
            # default spans a cat (25-42) and an adult (10-20) at once; narrow
            # it when the subject is known.
            cv.Optional(CONF_RATE_MIN_BPM, default=12.0): cv.float_range(4, 60),
            cv.Optional(CONF_RATE_MAX_BPM, default=66.0): cv.float_range(10, 150),
            cv.Optional(CONF_HIGHPASS_HZ, default=0.10): cv.float_range(0.02, 0.5),
            cv.Optional(CONF_STABILITY_WINDOW_SECONDS, default=30): cv.int_range(10, 60),
            cv.Optional(CONF_STABILITY_TOLERANCE_BPM, default=3.0): cv.float_range(1, 10),
            cv.Optional(CONF_STABILITY_THRESHOLD_PCT, default=60.0): cv.float_range(0, 100),
            cv.Optional(CONF_MIN_SNR_DB, default=6.0): cv.float_range(0, 20),
            # No default worth having: an honest floor has to be measured on
            # the installation. Zero disables the gate.
            cv.Optional(CONF_MIN_DEPTH_UM, default=0.0): cv.float_range(0, 5000),
            # Which of the 8 range rows to include. Row r covers
            # (r + 8) * 5.74 cm, so 0-7 is 45.9 to 86.1 cm.
            cv.Optional(CONF_RANGE_ROW_MIN, default=0): cv.int_range(0, 7),
            cv.Optional(CONF_RANGE_ROW_MAX, default=7): cv.int_range(0, 7),
            # 0 means "start the next analysis as soon as the last finished",
            # which is roughly 2 Hz, not the 4.88 Hz tile rate: one pass costs
            # about 450 ms on this chip.
            cv.Optional(CONF_UPDATE_SECONDS, default=10): cv.int_range(0, 3600),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_window_s(config[CONF_WINDOW_SECONDS]))
    cg.add(var.set_band_min_bpm(config[CONF_RATE_MIN_BPM]))
    cg.add(var.set_band_max_bpm(config[CONF_RATE_MAX_BPM]))
    cg.add(var.set_highpass_hz(config[CONF_HIGHPASS_HZ]))
    cg.add(var.set_stability_window_s(config[CONF_STABILITY_WINDOW_SECONDS]))
    cg.add(var.set_stability_tolerance_bpm(config[CONF_STABILITY_TOLERANCE_BPM]))
    cg.add(var.set_stability_threshold_pct(config[CONF_STABILITY_THRESHOLD_PCT]))
    cg.add(var.set_min_snr_db(config[CONF_MIN_SNR_DB]))
    cg.add(var.set_min_depth_um(config[CONF_MIN_DEPTH_UM]))
    cg.add(var.set_range_rows(config[CONF_RANGE_ROW_MIN],
                              config[CONF_RANGE_ROW_MAX]))
    cg.add(var.set_update_interval_s(config[CONF_UPDATE_SECONDS]))
