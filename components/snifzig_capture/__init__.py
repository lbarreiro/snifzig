import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32", "wifi"]
CONF_CHANNEL = "channel"
CONF_NTFY_PUBLISH_URL = "ntfy_publish_url"
CONF_NTFY_AUTHORIZATION = "ntfy_authorization"

snifzig_ns = cg.esphome_ns.namespace("snifzig_capture")
SnifzigCapture = snifzig_ns.class_("SnifzigCapture", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SnifzigCapture),
        cv.Optional(CONF_CHANNEL, default=11): cv.int_range(min=11, max=26),
        cv.Required(CONF_NTFY_PUBLISH_URL): cv.string,
        cv.Optional(CONF_NTFY_AUTHORIZATION, default=""): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    include_builtin_idf_component("ieee802154")
    include_builtin_idf_component("esp_http_client")
    include_builtin_idf_component("mbedtls")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(var.set_ntfy_publish_url(config[CONF_NTFY_PUBLISH_URL]))
    cg.add(var.set_ntfy_authorization(config[CONF_NTFY_AUTHORIZATION]))
