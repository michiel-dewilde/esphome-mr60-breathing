"""Whether a stable breathing pattern is present.

True only when the window-agreement, SNR and depth gates all hold. This is the
same decision that governs whether the rate sensor publishes a number or
`unknown`, exposed as a boolean so automations can act on presence of a
pattern without parsing the status string.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import CONF_MR60_BREATHING_ID, MR60Breathing

DEPENDENCIES = ["mr60_breathing"]

CONF_BREATHING_DETECTED = "breathing_detected"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_MR60_BREATHING_ID): cv.use_id(MR60Breathing),
        cv.Optional(CONF_BREATHING_DETECTED): binary_sensor.binary_sensor_schema(
            icon="mdi:lungs",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_MR60_BREATHING_ID])
    if CONF_BREATHING_DETECTED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_BREATHING_DETECTED])
        cg.add(hub.set_breathing_binary_sensor(sens))
