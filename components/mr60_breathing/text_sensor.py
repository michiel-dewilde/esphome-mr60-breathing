"""Why the breathing rate is, or is not, being reported.

One of: ok, unstable, low_snr, too_shallow, no_data, warming_up.

This exists because the rate sensor publishes `unknown` when it cannot measure
reliably, and `unknown` on its own does not distinguish "nothing is there" from
"something is there but moving too little to trust". Knowing which is the
difference between adjusting a threshold and repositioning the sensor.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_MR60_BREATHING_ID, MR60Breathing

DEPENDENCIES = ["mr60_breathing"]

CONF_STATUS = "status"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_MR60_BREATHING_ID): cv.use_id(MR60Breathing),
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:information-outline",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_MR60_BREATHING_ID])
    if CONF_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(hub.set_status_text_sensor(sens))
