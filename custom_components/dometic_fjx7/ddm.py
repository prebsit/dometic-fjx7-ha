"""DDM protocol encode/decode for Dometic FJX7 BLE communication.

This module has zero Home Assistant dependencies — it can be tested
standalone with pytest or used from a plain bleak script.

Frame format (validated against live device 2026-04-09):
    Byte 0:    Command type (0x10=Report, 0x11=Set, 0x12=Subscribe)
    Byte 1:    Parameter ID
    Byte 2:    0x00 (reserved padding — present in ALL command types)
    Byte 3-4:  Group ID (group_lo, group_hi)
    Byte 5-8:  Value as LE uint32 (Set/Report only; Subscribe has no value)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .const import (
    CMD_REPORT,
    CMD_SET,
    CMD_SUBSCRIBE,
    GRP_CLIMATE,
    PARAM_AC_MODE,
    PARAM_FAN_SPEED,
    PARAM_FAN_SPEED_PCT,
    PARAM_INTERIOR_LIGHT,
    PARAM_EXTERIOR_LIGHT,
    PARAM_MEASURED_TEMP,
    PARAM_POWER,
    PARAM_TARGET_TEMP,
)

HEADER_LEN = 5  # cmd + param + 0x00 + grp_lo + grp_hi
FRAME_LEN_REPORT = 9  # header + 4-byte value
FRAME_LEN_SET = 9
FRAME_LEN_SUBSCRIBE = 5  # header only, no value


@dataclass
class DDMReport:
    """A decoded report notification from the device."""

    param_id: int
    group: tuple[int, int]
    raw_value: int

    @property
    def is_climate(self) -> bool:
        return self.group == GRP_CLIMATE

    @property
    def temperature(self) -> float | None:
        """Decode millidegree value to °C, if this is a temp param."""
        if self.param_id in (PARAM_TARGET_TEMP, PARAM_MEASURED_TEMP):
            return self.raw_value / 1000.0
        return None

    @property
    def as_bool(self) -> bool:
        return self.raw_value != 0


def decode_report(data: bytes | bytearray) -> DDMReport | None:
    """Decode a BLE notification frame into a DDMReport.

    Returns None if the frame is too short or not a report.
    """
    if len(data) < HEADER_LEN:
        return None

    cmd = data[0]
    if cmd != CMD_REPORT:
        return None

    param_id = data[1]
    group = (data[3], data[4])

    raw_value = 0
    if len(data) >= FRAME_LEN_REPORT:
        raw_value = struct.unpack_from("<I", data, 5)[0]
    elif len(data) > HEADER_LEN:
        raw_value = data[5]

    return DDMReport(param_id=param_id, group=group, raw_value=raw_value)


def encode_subscribe(param_id: int, group: tuple[int, int] = GRP_CLIMATE) -> bytes:
    """Build a subscribe/get command."""
    return bytes([CMD_SUBSCRIBE, param_id, 0x00, *group])


def encode_set(
    param_id: int,
    value: int,
    group: tuple[int, int] = GRP_CLIMATE,
) -> bytes:
    """Build a set command with a uint32 value."""
    return bytes([CMD_SET, param_id, 0x00, *group]) + struct.pack("<I", value)


def encode_set_temperature(celsius: float) -> bytes:
    """Encode a target temperature set command (millidegrees)."""
    return encode_set(PARAM_TARGET_TEMP, int(celsius * 1000))


def encode_set_ac_mode(mode: int) -> bytes:
    """Encode an AC mode set command."""
    return encode_set(PARAM_AC_MODE, mode)


def encode_set_fan_speed(speed: int) -> bytes:
    """Encode a fan speed set command."""
    return encode_set(PARAM_FAN_SPEED, speed)


def encode_set_power(on: bool) -> bytes:
    """Encode a power on/off command."""
    return encode_set(PARAM_POWER, 1 if on else 0)


def encode_set_light(param_id: int, on: bool) -> bytes:
    """Encode an interior or exterior light command."""
    assert param_id in (PARAM_INTERIOR_LIGHT, PARAM_EXTERIOR_LIGHT)
    return encode_set(param_id, 1 if on else 0)
