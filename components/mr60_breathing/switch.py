"""Runtime control over the raw capture listener.

The listener is inert without a client, so turning this off is not about
overhead - it removes the open port entirely, for installations where an
unused listening socket is unwelcome.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_MR60_BREATHING_ID, MR60Breathing, mr60_breathing_ns

DEPENDENCIES = ["mr60_breathing"]

RawCaptureSwitch = mr60_breathing_ns.class_("RawCaptureSwitch", switch.Switch)

CONF_RAW_CAPTURE = "raw_capture"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_MR60_BREATHING_ID): cv.use_id(MR60Breathing),
        cv.Optional(CONF_RAW_CAPTURE): switch.switch_schema(
            RawCaptureSwitch,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:record-rec",
            default_restore_mode="RESTORE_DEFAULT_ON",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_MR60_BREATHING_ID])
    if CONF_RAW_CAPTURE in config:
        sw = await switch.new_switch(config[CONF_RAW_CAPTURE])
        cg.add(sw.set_parent(hub))
