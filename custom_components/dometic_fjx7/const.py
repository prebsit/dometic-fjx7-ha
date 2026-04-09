"""Constants for the Dometic FJX7 integration."""

from __future__ import annotations

DOMAIN = "dometic_fjx7"
MANUFACTURER = "Dometic"

# ── BLE identifiers ──────────────────────────────────────────────
SERVICE_UUID = "537a0400-0995-481f-926c-1604e23fd515"
WRITE_UUID = "537a0401-0995-481f-926c-1604e23fd515"
NOTIFY_UUID = "537a0402-0995-481f-926c-1604e23fd515"

# Device advertises as SHE_XXXXXX (last 6 of BLE MAC)
DEVICE_NAME_PREFIX = "SHE_"

# ── DDM command types ────────────────────────────────────────────
CMD_REPORT = 0x10  # device → host
CMD_SET = 0x11  # host → device
CMD_SUBSCRIBE = 0x12  # host → device

# ── DDM groups ───────────────────────────────────────────────────
GRP_CLIMATE = (0x02, 0x01)  # Group 0x0201 — climate zone 1
GRP_DEVICE = (0x00, 0x00)  # Group 0x0000 — device info

# ── Climate parameter IDs (group 0x0201) ─────────────────────────
PARAM_POWER = 0x01
PARAM_FAN_SPEED = 0x02
PARAM_AC_MODE = 0x03
PARAM_TARGET_TEMP = 0x04
PARAM_INTERIOR_LIGHT = 0x05
PARAM_FAN_SPEED_PCT = 0x06  # read-only, 0-100
PARAM_UNKNOWN_09 = 0x09
PARAM_MEASURED_TEMP = 0x0A  # read-only
PARAM_EXTERIOR_LIGHT = 0x0E
PARAM_UNKNOWN_1B = 0x1B
PARAM_UNKNOWN_1C = 0x1C

# Essential params first — connection may drop mid-subscribe
SUBSCRIBE_PARAMS = [
    PARAM_POWER,
    PARAM_TARGET_TEMP,
    PARAM_MEASURED_TEMP,
    PARAM_AC_MODE,
    PARAM_FAN_SPEED,
    PARAM_FAN_SPEED_PCT,
    PARAM_INTERIOR_LIGHT,
    PARAM_EXTERIOR_LIGHT,
    PARAM_UNKNOWN_1B,
    PARAM_UNKNOWN_09,
]

# ── AC mode mapping ──────────────────────────────────────────────
# DDM value → HA HVACMode string
AC_MODE_DRY = 0
AC_MODE_HEAT = 1
AC_MODE_FAN_ONLY = 2  # "Ventilation" in Dometic app
AC_MODE_AUTO = 3
AC_MODE_COOL = 5

# ── Fan speed mapping ────────────────────────────────────────────
FAN_SPEED_AUTO = 0
FAN_SPEED_LOW = 1
FAN_SPEED_MEDIUM = 2
FAN_SPEED_HIGH = 3
FAN_SPEED_TURBO = 4

FAN_SPEED_NAMES = {
    FAN_SPEED_AUTO: "auto",
    FAN_SPEED_LOW: "low",
    FAN_SPEED_MEDIUM: "medium",
    FAN_SPEED_HIGH: "high",
    FAN_SPEED_TURBO: "turbo",
}
FAN_SPEED_FROM_NAME = {v: k for k, v in FAN_SPEED_NAMES.items()}

# ── Temperature limits ───────────────────────────────────────────
TEMP_MIN = 16.0
TEMP_MAX = 31.0
TEMP_STEP = 0.5
