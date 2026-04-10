import esphome.codegen as cg
from esphome import automation
from esphome.components import display, socket
from esphome.components.display import DisplayRef
from esphome.components.usb_host import register_usb_client, usb_device_schema
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    only_on_variant,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_LAMBDA, CONF_MODEL
from esphome.cpp_types import Component

AUTO_LOAD = ["usb_host", "display", "bytebuffer", "socket"]
CODEOWNERS = ["@rhooper"]
DEPENDENCIES = ["esp32"]

usb_printer_ns = cg.esphome_ns.namespace("usb_printer")
LabelBuffer = usb_printer_ns.class_("LabelBuffer", display.DisplayBuffer)
BrotherQLPrinter = usb_printer_ns.class_("BrotherQLPrinter", Component)
PrintAction = usb_printer_ns.class_("PrintAction", automation.Action)

CONF_VID = "vid"
CONF_PID = "pid"
CONF_LABEL = "label"
CONF_LABEL_BUFFER_ID = "label_buffer_id"

# Brother QL label definitions: (printable_w, printable_h, width_mm, height_mm, media_type, feed_margin)
# media_type: 0x0A = endless, 0x0B = die-cut
BROTHER_QL_LABELS = {
    "29": (306, 0, 29, 0, 0x0A, 35),
    "38": (413, 0, 38, 0, 0x0A, 35),
    "50": (554, 0, 50, 0, 0x0A, 35),
    "54": (590, 0, 54, 0, 0x0A, 35),
    "62": (696, 0, 62, 0, 0x0A, 35),
    "29x90": (306, 991, 29, 90, 0x0B, 0),
    "29x42": (306, 425, 29, 42, 0x0B, 0),
    "39x48": (495, 578, 39, 48, 0x0B, 0),
    "52x29": (578, 271, 52, 29, 0x0B, 0),
    "62x29": (696, 271, 62, 29, 0x0B, 0),
    "62x100": (696, 1109, 62, 100, 0x0B, 0),
    "17x54": (165, 566, 17, 54, 0x0B, 0),
    "17x87": (165, 956, 17, 87, 0x0B, 0),
}

BROTHER_QL_MODELS = {
    "QL-500": {"bytes_per_row": 90, "compression": False, "cutting": False},
    "QL-550": {"bytes_per_row": 90, "compression": False, "cutting": True},
    "QL-560": {"bytes_per_row": 90, "compression": False, "cutting": True},
    "QL-570": {"bytes_per_row": 90, "compression": False, "cutting": True},
    "QL-580N": {"bytes_per_row": 90, "compression": True, "cutting": True},
    "QL-700": {"bytes_per_row": 90, "compression": False, "cutting": True},
    "QL-710W": {"bytes_per_row": 90, "compression": True, "cutting": True},
    "QL-720NW": {"bytes_per_row": 90, "compression": True, "cutting": True},
    "QL-800": {"bytes_per_row": 90, "compression": True, "cutting": True},
    "QL-810W": {"bytes_per_row": 90, "compression": True, "cutting": True},
    "QL-820NWB": {"bytes_per_row": 90, "compression": True, "cutting": True},
    "QL-1050": {"bytes_per_row": 162, "compression": True, "cutting": True},
    "QL-1060N": {"bytes_per_row": 162, "compression": True, "cutting": True},
}


def validate_label(value):
    value = str(value)
    if value not in BROTHER_QL_LABELS:
        raise cv.Invalid(
            f"Unknown label type '{value}'. Must be one of: {', '.join(BROTHER_QL_LABELS.keys())}"
        )
    return value


def validate_model(value):
    value = str(value)
    if value not in BROTHER_QL_MODELS:
        raise cv.Invalid(
            f"Unknown model '{value}'. Must be one of: {', '.join(BROTHER_QL_MODELS.keys())}"
        )
    return value


class Type:
    def __init__(self, name, vid, pid, cls):
        self.name = name
        self.vid = vid
        self.pid = pid
        self.cls = cls


printer_types = (
    Type("BROTHER_QL", 0x04F9, 0x2015, BrotherQLPrinter),
)


def printer_schema(printer_type):
    return usb_device_schema(printer_type.cls, printer_type.vid, printer_type.pid).extend(
        {
            cv.GenerateID(CONF_LABEL_BUFFER_ID): cv.declare_id(LabelBuffer),
            cv.Required(CONF_MODEL): validate_model,
            cv.Required(CONF_LABEL): validate_label,
            cv.Optional(CONF_LAMBDA): cv.lambda_,
        }
    )


CONFIG_SCHEMA = cv.All(
    cv.ensure_list(
        cv.typed_schema(
            {it.name: printer_schema(it) for it in printer_types},
            upper=True,
        )
    ),
    only_on_variant(supported=[VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3]),
)


async def to_code(config):
    socket.require_wake_loop_threadsafe()

    for device in config:
        var = await register_usb_client(device)

        model = device[CONF_MODEL]
        label = device[CONF_LABEL]
        label_info = BROTHER_QL_LABELS[label]
        model_info = BROTHER_QL_MODELS[model]

        # Create the LabelBuffer (separate DisplayBuffer for rendering)
        label_buf = cg.new_Pvariable(
            device[CONF_LABEL_BUFFER_ID],
            label_info[0],  # printable_w
            label_info[1],  # printable_h
        )
        await cg.register_component(label_buf, {})

        cg.add(var.set_label_buffer(label_buf))
        cg.add(var.set_model_info(
            model_info["bytes_per_row"],
            model_info["compression"],
            model_info["cutting"],
        ))
        cg.add(var.set_label_info(
            label_info[0],  # printable_w
            label_info[1],  # printable_h
            label_info[2],  # width_mm
            label_info[3],  # height_mm
            label_info[4],  # media_type
            label_info[5],  # feed_margin
        ))

        if CONF_LAMBDA in device:
            lambda_ = await cg.process_lambda(
                device[CONF_LAMBDA], [(DisplayRef, "it")], return_type=cg.void
            )
            cg.add(var.set_writer(lambda_))


@automation.register_action(
    "usb_printer.print",
    PrintAction,
    automation.maybe_simple_id(
        {
            cv.Required(CONF_ID): cv.use_id(BrotherQLPrinter),
        }
    ),
    synchronous=True,
)
async def print_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
