"""ESPHome external component: Corsair Commander Pro Emulation.

Emulates a Corsair Commander Pro (VID: 0x1B1C, PID: 0x0C10) via TinyUSB
to integrate natively with the Linux kernel 'corsair-cpro' hwmon module.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.const import CONF_ID

CODEOWNERS = ["@custom"]

corsair_cpro_ns = cg.esphome_ns.namespace("corsair_cpro")
CorsairCPro = corsair_cpro_ns.class_("CorsairCPro", cg.Component)

CONF_SERIAL_NUMBER = "serial_number"

CONF_TEMPS = [f"temp_{i}" for i in range(1, 5)]
CONF_VOLTS = ["volt_12v", "volt_5v", "volt_3v3"]
CONF_FANS = [f"fan_{i}_rpm" for i in range(1, 7)]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CorsairCPro),
        cv.Optional(CONF_SERIAL_NUMBER): cv.string,
        **{cv.Optional(k): cv.use_id(sensor.Sensor) for k in CONF_TEMPS},
        **{cv.Optional(k): cv.use_id(sensor.Sensor) for k in CONF_VOLTS},
        **{cv.Optional(k): cv.use_id(sensor.Sensor) for k in CONF_FANS},
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_SERIAL_NUMBER in config:
        cg.add(var.set_serial_number(config[CONF_SERIAL_NUMBER]))

    # Temperature channels 1..4 (indices 0..3)
    for ch, key in enumerate(CONF_TEMPS):
        if key in config:
            cg.add(var.set_temp_connected(ch, True))
            sens = await cg.get_variable(config[key])
            cg.add(
                sens.add_on_state_callback(
                    cg.RawExpression(f"[](float x) {{ {var}->set_temperature({ch}, x); }}")
                )
            )

    # Voltage rails (12V, 5V, 3.3V)
    for rail, key in enumerate(CONF_VOLTS):
        if key in config:
            sens = await cg.get_variable(config[key])
            cg.add(
                sens.add_on_state_callback(
                    cg.RawExpression(f"[](float x) {{ {var}->set_voltage({rail}, x); }}")
                )
            )

    # Fan channels 1..6 (indices 0..5)
    for ch, key in enumerate(CONF_FANS):
        if key in config:
            cg.add(var.set_fan_connected(ch, 2))  # 2 = 4-pin PWM mode
            sens = await cg.get_variable(config[key])
            cg.add(
                sens.add_on_state_callback(
                    cg.RawExpression(f"[](float x) {{ {var}->set_fan_rpm({ch}, (uint16_t)x); }}")
                )
            )

    # Pull in TinyUSB stack and enable 1 HID interface
    add_idf_component(name="espressif/esp_tinyusb", ref="2.2.1")
    add_idf_sdkconfig_option("CONFIG_TINYUSB_HID_COUNT", 1)
