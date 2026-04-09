"""BLE client for Dometic FJX7.

Manages BLE connection, notification subscriptions, and state.
Uses a connect-operate-disconnect pattern for reliability on
Pi 3B+ bcm43438 (known BLE stability issues with persistent connections).
"""

from __future__ import annotations

import asyncio
import logging
from typing import Callable

from bleak import BleakClient, BleakError
from bleak.backends.device import BLEDevice

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
    """Manages BLE communication with a Dometic FJX7.

    Uses a connect-subscribe-collect-disconnect pattern rather than
    holding a persistent connection. The Pi 3B+ bcm43438 has known
    BLE stability issues with long-lived connections.
    """

    def __init__(
        self,
        ble_device: BLEDevice,
        state_callback: Callable[[], None] | None = None,
    ) -> None:
        self._ble_device = ble_device
        self._state_callback = state_callback
        self.state = FJX7State()
        self._connected = False

    @property
    def is_connected(self) -> bool:
        return self._connected

    def set_ble_device(self, ble_device: BLEDevice) -> None:
        """Update the BLE device reference (e.g. after rediscovery)."""
        self._ble_device = ble_device

    async def async_poll_state(self) -> bool:
        """Connect, subscribe, collect state, disconnect. Returns True on success."""
        address = self._ble_device.address
        _LOGGER.info("FJX7: poll starting for %s (%s)", self._ble_device.name, address)

        notifications: list[bytearray] = []

        def on_notify(_sender: int, data: bytearray) -> None:
            notifications.append(data)

        client = None
        try:
            _LOGGER.debug("FJX7: creating BleakClient with BLEDevice")
            client = BleakClient(self._ble_device, timeout=15.0)

            _LOGGER.debug("FJX7: calling connect()")
            await client.connect()
            self._connected = True
            _LOGGER.info("FJX7: connected! services: %d", len(client.services.services))

            _LOGGER.debug("FJX7: starting notifications")
            await client.start_notify(NOTIFY_UUID, on_notify)

            _LOGGER.debug("FJX7: subscribing to params")
            success_count = 0
            for i, param in enumerate(SUBSCRIBE_PARAMS):
                cmd = encode_subscribe(param)
                _LOGGER.debug("FJX7: writing subscribe %d/%d param=0x%02x", i+1, len(SUBSCRIBE_PARAMS), param)
                try:
                    await client.write_gatt_char(WRITE_UUID, cmd, response=False)
                    success_count += 1
                except Exception as write_err:
                    _LOGGER.debug("FJX7: subscribe write failed at %d/%d: %s", i+1, len(SUBSCRIBE_PARAMS), write_err)
                    break
                await asyncio.sleep(0.05)

            _LOGGER.debug("FJX7: %d/%d subscribes sent, waiting for notifications", success_count, len(SUBSCRIBE_PARAMS))
            await asyncio.sleep(0.5)

            await client.stop_notify(NOTIFY_UUID)
            await client.disconnect()
            self._connected = False

            # Process collected notifications
            changed = False
            for data in notifications:
                report = decode_report(data)
                if report and report.is_climate:
                    if self.state.update_from_report(report):
                        changed = True

            if changed and self._state_callback:
                self._state_callback()

            _LOGGER.info(
                "FJX7: poll complete, got %d notifications, state changed: %s",
                len(notifications), changed,
            )
            return len(notifications) > 0

        except Exception as err:
            self._connected = False
            _LOGGER.warning("FJX7: poll failed: %s: %s", type(err).__name__, err)
            if client:
                try:
                    if client.is_connected:
                        await client.disconnect()
                except Exception:
                    pass
            return False

    async def async_send_command(self, data: bytes) -> bool:
        """Connect, send a single command, wait for echo, disconnect."""
        address = self._ble_device.address
        _LOGGER.info("FJX7: sending command %s to %s", data.hex(), address)

        echo_received = asyncio.Event()
        echo_data: list[bytearray] = []

        def on_notify(_sender: int, ndata: bytearray) -> None:
            echo_data.append(ndata)
            report = decode_report(ndata)
            if report and report.is_climate:
                self.state.update_from_report(report)
                echo_received.set()

        client = None
        try:
            client = BleakClient(self._ble_device, timeout=15.0)
            await client.connect()
            self._connected = True

            await client.start_notify(NOTIFY_UUID, on_notify)
            await client.write_gatt_char(WRITE_UUID, data, response=False)
            _LOGGER.debug("FJX7: command sent, waiting for echo")

            try:
                await asyncio.wait_for(echo_received.wait(), timeout=3.0)
            except asyncio.TimeoutError:
                _LOGGER.debug("FJX7: no echo within 3s")

            await client.stop_notify(NOTIFY_UUID)
            await client.disconnect()
            self._connected = False

            if self._state_callback and echo_data:
                self._state_callback()

            _LOGGER.info("FJX7: command complete, %d echoes", len(echo_data))
            return True

        except Exception as err:
            self._connected = False
            _LOGGER.warning("FJX7: command failed: %s: %s", type(err).__name__, err)
            if client:
                try:
                    if client.is_connected:
                        await client.disconnect()
                except Exception:
                    pass
            return False

    async def async_set_temperature(self, celsius: float) -> bool:
        return await self.async_send_command(encode_set_temperature(celsius))

    async def async_set_ac_mode(self, mode: int) -> bool:
        return await self.async_send_command(encode_set(PARAM_AC_MODE, mode))

    async def async_set_fan_speed(self, speed: int) -> bool:
        return await self.async_send_command(encode_set(PARAM_FAN_SPEED, speed))

    async def async_set_power(self, on: bool) -> bool:
        return await self.async_send_command(encode_set(PARAM_POWER, 1 if on else 0))

    async def async_set_light(self, param_id: int, on: bool) -> bool:
        return await self.async_send_command(encode_set(param_id, 1 if on else 0))

    async def connect(self) -> None:
        """Initial state poll on startup."""
        await self.async_poll_state()

    async def disconnect(self) -> None:
        """No-op — connections are already short-lived."""
        self._connected = False
