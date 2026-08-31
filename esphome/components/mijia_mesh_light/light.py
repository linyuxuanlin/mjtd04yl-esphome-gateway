import re

import esphome.codegen as cg
from esphome.components import binary_sensor, ble_client, light
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_OUTPUT_ID,
)

DEPENDENCIES = ["ble_client", "light"]
AUTO_LOAD = ["binary_sensor"]

CONF_LTMK = "ltmk"
CONF_AUTHENTICATED = "authenticated"


def validate_ltmk(value):
    value = cv.string_strict(value).strip()
    if not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise cv.Invalid("ltmk must be exactly 32 bytes encoded as 64 hexadecimal characters")
    return value.lower()


mijia_mesh_light_ns = cg.esphome_ns.namespace("mijia_mesh_light")
MijiaMeshLight = mijia_mesh_light_ns.class_(
    "MijiaMeshLight", cg.Component, ble_client.BLEClientNode, light.LightOutput
)

CONFIG_SCHEMA = (
    light.BRIGHTNESS_ONLY_LIGHT_SCHEMA.extend(
        {
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(MijiaMeshLight),
            cv.Required(CONF_LTMK): validate_ltmk,
            cv.Optional(CONF_AUTHENTICATED): binary_sensor.binary_sensor_schema(
                icon="mdi:shield-check"
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_ltmk(config[CONF_LTMK]))

    if CONF_AUTHENTICATED in config:
        sensor = await binary_sensor.new_binary_sensor(config[CONF_AUTHENTICATED])
        cg.add(var.set_authenticated_sensor(sensor))

    await light.register_light(var, config)
