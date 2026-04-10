"""BLE client for Dometic FJX7.

Manages BLE connection, notification subscriptions, and state.
Uses a connect-operate-disconnect pattern for reliability.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Callable

from bleak import BleakClient, BleakError
from bleak.backends.device import BLEDevice
from bleak_retry_connector import establish_connection

from .const import (
    NOTIFY_UUID,
    PARAM_AC_MODE,
    PARAM_EXTERIOR_LIGHT,
    PARAM_FAN_SPEED,
    PARAM_FAN_SPEED_PCT,
    PARAM_INTERIOR_LIGHT,
    PARAM_MEASURED_TEMP,
    PARAM_POWER,
    PARAM_TARGET_TEMP,
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
        return all(
            p in self._raw
            for p in (PARAM_POWER, PARAM_TARGET_TEMP, PARAM_MEASURED_TEMP)
        )


class FJX7BLEClient:
    """Manages BLE communication with a Dometic FJX7."""

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
        self._ble_device = ble_device

    async def async_poll_state(self) -> bool:
        """Connect, subscribe, collect state, disconnect."""
        address = self._ble_device.address
        _LOGGER.info("FJX7: poll starting for %s (%s)", self._ble_device.name, address)

        notifications: list[bytearray] = []

        def on_notify(_sender: int, data: bytearray) -> None:
            _LOGGER.debug("FJX7: notification received: %s", data.hex())
            notifications.append(data)

        client = None
        try:
            client = await establish_connection(
                BleakClient,
                self._ble_device,
                self._ble_device.name or "FJX7",
                max_attempts=1,
                ble_device_callback=lambda: self._ble_device,
            )
            self._connected = True
            _LOGGER.info("FJX7: connected! services: %d", len(client.services.services))

            # Log all services and characteristics for debugging
            for svc in client.services:
                _LOGGER.debug("FJX7: service: %s", svc.uuid)
                for char in svc.characteristics:
                    _LOGGER.debug("FJX7:   char: %s props=%s handle=0x%04x", char.uuid, char.properties, char.handle)

            # Test: try a simple read first
            try:
                val = await client.read_gatt_char(NOTIFY_UUID)
                _LOGGER.info("FJX7: test read OK: %s", val.hex() if val else "empty")
            except Exception as read_err:
                _LOGGER.debug("FJX7: test read failed: %s: %s", type(read_err).__name__, read_err)

            # Write subscribes BEFORE enabling notifications
            success_count = 0
            for i, param in enumerate(SUBSCRIBE_PARAMS):
                cmd = encode_subscribe(param)
                _LOGGER.debug("FJX7: writing subscribe %d/%d param=0x%02x", i+1, len(SUBSCRIBE_PARAMS), param)
                try:
                    await client.write_gatt_char(WRITE_UUID, cmd, response=True)
                    success_count += 1
                except Exception as write_err:
                    _LOGGER.debug("FJX7: subscribe write failed at %d/%d: %s: %s", i+1, len(SUBSCRIBE_PARAMS), type(write_err).__name__, write_err)
                    break

            _LOGGER.debug("FJX7: %d/%d subscribes sent, now enabling notifications", success_count, len(SUBSCRIBE_PARAMS))
            await client.start_notify(NOTIFY_UUID, on_notify)
            _LOGGER.debug("FJX7: notifications enabled, waiting for data")
            await asyncio.sleep(2.0)

            try:
                await client.stop_notify(NOTIFY_UUID)
            except Exception:
                pass
            try:
                await client.disconnect()
            except Exception:
                pass
            self._connected = False

            changed = False
            for data in notifications:
                report = decode_report(data)
                if report and report.is_climate:
                    if self.state.update_from_report(report):
                        changed = True

            if changed and self._state_callback:
                self._state_callback()

            _LOGGER.info("FJX7: poll complete, %d/%d subscribes, %d notifications", success_count, len(SUBSCRIBE_PARAMS), len(notifications))
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
        _LOGGER.info("FJX7: sending command %s", data.hex())

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
            client = await establish_connection(
                BleakClient,
                self._ble_device,
                self._ble_device.name or "FJX7",
                max_attempts=1,
                ble_device_callback=lambda: self._ble_device,
            )
            self._connected = True

            await client.start_notify(NOTIFY_UUID, on_notify)
            await client.write_gatt_char(WRITE_UUID, data, response=True)
            _LOGGER.debug("FJX7: command sent, waiting for echo")

            try:
                await asyncio.wait_for(echo_received.wait(), timeout=3.0)
            except asyncio.TimeoutError:
                _LOGGER.debug("FJX7: no echo within 3s")

            try:
                await client.stop_notify(NOTIFY_UUID)
            except Exception:
                pass
            try:
                await client.disconnect()
            except Exception:
                pass
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
        await self.async_poll_state()

    async def disconnect(self) -> None:
        self._connected = False
