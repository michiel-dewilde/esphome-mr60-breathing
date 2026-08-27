"""Sensors for the MR60BHA2 breathing component.

breathing_rate publishes `unknown` rather than a number whenever the verdict
does not hold. The diagnostics alongside it tell you whether the radar link is
healthy, which matters more than it sounds: collection mode runs at about 88 %
of the UART's capacity, so degradation should be visible rather than silent. A
sample rate that has drifted below 4.88 Hz means tile sets are being lost.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_DECIBEL,
    UNIT_HERTZ,
    UNIT_PERCENT,
)

from . import CONF_MR60_BREATHING_ID, MR60Breathing

DEPENDENCIES = ["mr60_breathing"]

CONF_BREATHING_RATE = "breathing_rate"
CONF_RATE_TIME_DOMAIN = "rate_time_domain"
CONF_STABILITY = "stability"
CONF_SNR = "snr"
CONF_DEPTH = "movement_depth"
CONF_SAMPLE_RATE = "sample_rate"
CONF_FRAME_ERRORS = "frame_errors"
CONF_TILE_SETS = "tile_sets"
CONF_BUFFERED_SECONDS = "buffered_seconds"

UNIT_SECONDS = "s"
UNIT_BREATHS_PER_MINUTE = "/min"
UNIT_MICROMETRE = "µm"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_MR60_BREATHING_ID): cv.use_id(MR60Breathing),
        cv.Optional(CONF_BREATHING_RATE): sensor.sensor_schema(
            unit_of_measurement=UNIT_BREATHS_PER_MINUTE,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:lungs",
        ),
        # The spectral estimate has run low against human counts on fast
        # breathing; when the two disagree the higher has been closer.
        cv.Optional(CONF_RATE_TIME_DOMAIN): sensor.sensor_schema(
            unit_of_measurement=UNIT_BREATHS_PER_MINUTE,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:chart-bell-curve",
        ),
        cv.Optional(CONF_STABILITY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:chart-line-variant",
        ),
        cv.Optional(CONF_SNR): sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:signal",
        ),
        # A lower bound on chest displacement, not a calibrated figure.
        cv.Optional(CONF_DEPTH): sensor.sensor_schema(
            unit_of_measurement=UNIT_MICROMETRE,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:arrow-expand-vertical",
        ),
        cv.Optional(CONF_SAMPLE_RATE): sensor.sensor_schema(
            unit_of_measurement=UNIT_HERTZ,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:speedometer",
        ),
        cv.Optional(CONF_FRAME_ERRORS): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:alert-circle-outline",
        ),
        cv.Optional(CONF_TILE_SETS): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:counter",
        ),
        cv.Optional(CONF_BUFFERED_SECONDS): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECONDS,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:history",
        ),
    }
)

_SETTERS = {
    CONF_BREATHING_RATE: "set_breathing_rate_sensor",
    CONF_RATE_TIME_DOMAIN: "set_rate_time_domain_sensor",
    CONF_STABILITY: "set_stability_sensor",
    CONF_SNR: "set_snr_sensor",
    CONF_DEPTH: "set_depth_sensor",
    CONF_SAMPLE_RATE: "set_sample_rate_sensor",
    CONF_FRAME_ERRORS: "set_frame_errors_sensor",
    CONF_TILE_SETS: "set_tile_sets_sensor",
    CONF_BUFFERED_SECONDS: "set_buffered_seconds_sensor",
}


async def to_code(config):
    hub = await cg.get_variable(config[CONF_MR60_BREATHING_ID])
    for key, setter in _SETTERS.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(hub, setter)(sens))
