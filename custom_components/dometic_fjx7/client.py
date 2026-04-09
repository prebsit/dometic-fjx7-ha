"""BLE client for Dometic FJX7.

Manages the BLE connection, notification subscriptions, and state.
Uses HA's BleakClientWrapper for transport abstraction (works with
local BLE adapter or ESPHome bluetooth_proxy transparently).
"""

from __future__ import annotations

import asyncio
import logging
from typing import Callable

from bleak import BleakError
from bleak.backends.device import BLEDevice
from bleak import BleakClient

from .const import (
    GRP_CLIMATE,
    NOTIFY_UUID,
    PARAM_AC_MODE,
    PARAM_EXTERIOR_LIGHT,
    PARAM_FAN_SPEED,
    PARAM_FAN_SPEED_PCT,
    PARAM_INTERIOR_LIGHT,
    PARAM_MEASURED_TEMP,
    PARAM_POWER,
    PARAM_TARGET_TEMP,
    PARAM_UNKNOWN_09,
    PARAM_UNKNOWN_1B,
    SUBSCRIBE_PARAMS,
    WRITE_UUID,
)
from .ddm import (
    DDMReport,
    decode_report,
    encode_set,
    encode_set_temperature,
    encode_subscribe,
)

_LOGGER = logging.getLogger(__name__)

WRITE_PACING = 0.15  # seconds between BLE writes (matches Dometic app)


class FJX7State:
    """Current state of the FJX7, populated from BLE notifications."""

    def __init__(self) -> None:
        self.power: bool = False
        self.ac_mode: int = 0
        self.fan_speed: int = 0
        self.target_temp: float = 16.0
        self.measured_temp: float = 0.0
        self.fan_speed_pct: int = 0
        self.interior_light: bool = False
        self.exterior_light: bool = False
        self._raw: dict[int, int] = {}

    def update_from_report(self, report: DDMReport) -> bool:
        """Apply a DDM report to state. Returns True if state changed."""
        if not report.is_climate:
            return False

        old = self._raw.get(report.param_id)
        self._raw[report.param_id] = report.raw_value

        if report.param_id == PARAM_POWER:
            self.power = report.as_bool
        elif report.param_id == PARAM_AC_MODE:
            self.ac_mode = report.raw_value
        elif report.param_id == PARAM_FAN_SPEED:
            self.fan_speed = report.raw_value
        elif report.param_id == PARAM_TARGET_TEMP:
            self.target_temp = report.temperature or 16.0
        elif report.param_id == PARAM_MEASURED_TEMP:
            self.measured_temp = report.temperature or 0.0
        elif report.param_id == PARAM_FAN_SPEED_PCT:
            self.fan_speed_pct = report.raw_value
        elif report.param_id == PARAM_INTERIOR_LIGHT:
            self.interior_light = report.as_bool
        elif report.param_id == PARAM_EXTERIOR_LIGHT:
            self.exterior_light = report.as_bool

        return old != report.raw_value

    @property
    def got_initial_state(self) -> bool:
        """True once we have the essential params."""
        return all(
            p in self._raw
            for p in (PARAM_POWER, PARAM_TARGET_TEMP, PARAM_MEASURED_TEMP)
        )


class FJX7BLEClient:
    """Manages BLE connection and communication with a Dometic FJX7."""

    def __init__(
        self,
        ble_device: BLEDevice,
        state_callback: Callable[[], None] | None = None,
    ) -> None:
        self._ble_device = ble_device
        self._client: BleakClient | None = None
        self._state_callback = state_callback
        self._disconnect_event = asyncio.Event()
        self.state = FJX7State()

    @property
    def is_connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    def set_ble_device(self, ble_device: BLEDevice) -> None:
        """Update the BLE device reference (e.g. after rediscovery)."""
        self._ble_device = ble_device

    async def connect(self) -> None:
        """Connect to the FJX7 and subscribe to climate notifications."""
        _LOGGER.debug("Connecting to %s", self._ble_device.name)

        self._client = BleakClient(
            self._ble_device,
            disconnected_callback=self._on_disconnect,
            timeout=30.0,
        )
        await self._client.connect()

        _LOGGER.debug("Connected, waiting for GATT services")
        await asyncio.sleep(1.0)  # let Pi BLE stack settle

        _LOGGER.debug("Enabling notifications")
        await self._client.start_notify(NOTIFY_UUID, self._on_notification)
        await asyncio.sleep(0.5)

        _LOGGER.debug("Subscribing to %d climate params", len(SUBSCRIBE_PARAMS))
        for param in SUBSCRIBE_PARAMS:
            cmd = encode_subscribe(param)
            try:
                await self._client.write_gatt_char(WRITE_UUID, cmd, response=True)
            except Exception as err:
                _LOGGER.warning("FJX7: subscribe param 0x%02x failed: %s", param, err)
                raise
            await asyncio.sleep(0.3)  # slower pacing for Pi BLE

        _LOGGER.info(
            "FJX7 %s: connected and subscribed", self._ble_device.name
        )

    async def disconnect(self) -> None:
        """Disconnect from the device."""
        if self._client and self._client.is_connected:
            try:
                await self._client.stop_notify(NOTIFY_UUID)
            except BleakError:
                pass
            await self._client.disconnect()
        self._client = None

    async def async_set_temperature(self, celsius: float) -> None:
        """Set target temperature."""
        await self._write(encode_set_temperature(celsius))

    async def async_set_ac_mode(self, mode: int) -> None:
        """Set AC operating mode."""
        await self._write(encode_set(PARAM_AC_MODE, mode))

    async def async_set_fan_speed(self, speed: int) -> None:
        """Set fan speed."""
        await self._write(encode_set(PARAM_FAN_SPEED, speed))

    async def async_set_power(self, on: bool) -> None:
        """Turn power on or off."""
        await self._write(encode_set(PARAM_POWER, 1 if on else 0))

    async def async_set_light(self, param_id: int, on: bool) -> None:
        """Set interior or exterior light."""
        await self._write(encode_set(param_id, 1 if on else 0))

    async def _write(self, data: bytes) -> None:
        """Write a command to the device."""
        if not self.is_connected:
            raise BleakError("Not connected to FJX7")
        _LOGGER.debug("BLE write: %s", data.hex())
        await self._client.write_gatt_char(WRITE_UUID, data, response=True)

    def _on_notification(self, _sender: int, data: bytearray) -> None:
        """Handle incoming BLE notification."""
        report = decode_report(data)
        if report is None:
            _LOGGER.debug("Undecodable frame: %s", data.hex())
            return

        changed = self.state.update_from_report(report)
        if changed and self._state_callback:
            self._state_callback()

    def _on_disconnect(self, _client: BleakClient) -> None:
        """Handle unexpected disconnection."""
        _LOGGER.warning("FJX7 %s: disconnected", self._ble_device.name)
        self._disconnect_event.set()
        self._client = None
