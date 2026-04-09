"""Coordinator for Dometic FJX7 BLE integration.

Manages the BLE client lifecycle, reconnection, and entity update
signalling. Entities register callbacks with the coordinator rather
than polling.
"""

from __future__ import annotations

import asyncio
import logging

from bleak import BleakError
from bleak.backends.device import BLEDevice

from homeassistant.components import bluetooth
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator

from .client import FJX7BLEClient, FJX7State
from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

RECONNECT_DELAY = 30  # seconds before reconnect attempt (FJX7 rejects rapid retries)


class FJX7Coordinator(DataUpdateCoordinator[FJX7State]):
    """Coordinate BLE communication with a Dometic FJX7."""

    def __init__(
        self,
        hass: HomeAssistant,
        address: str,
    ) -> None:
        """Initialise coordinator."""
        super().__init__(
            hass,
            _LOGGER,
            name=f"{DOMAIN}_{address}",
            # No polling interval — we use BLE notifications (push)
            update_interval=None,
        )
        self.address = address
        self._client: FJX7BLEClient | None = None
        self._reconnect_task: asyncio.Task | None = None

    @property
    def client(self) -> FJX7BLEClient | None:
        return self._client

    @property
    def state(self) -> FJX7State:
        """Shortcut to current device state."""
        if self._client:
            return self._client.state
        return FJX7State()

    @property
    def is_connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    async def async_start(self) -> None:
        """Start the coordinator — connect to device."""
        ble_device = bluetooth.async_ble_device_from_address(
            self.hass, self.address, connectable=True
        )
        if ble_device is None:
            _LOGGER.error("FJX7 %s not found via Bluetooth", self.address)
            return

        await self._connect(ble_device)

    async def async_stop(self) -> None:
        """Stop the coordinator — disconnect."""
        if self._reconnect_task:
            self._reconnect_task.cancel()
            self._reconnect_task = None
        if self._client:
            await self._client.disconnect()
            self._client = None

    async def _connect(self, ble_device: BLEDevice) -> None:
        """Establish BLE connection."""
        self._client = FJX7BLEClient(
            ble_device,
            state_callback=self._on_state_changed,
        )
        try:
            await self._client.connect()
            self.async_set_updated_data(self._client.state)
            _LOGGER.info("FJX7 %s: connected", self.address)
        except (BleakError, TimeoutError) as err:
            _LOGGER.warning("FJX7 %s: connection failed: %s", self.address, err)
            self._schedule_reconnect()

    @callback
    def _on_state_changed(self) -> None:
        """Called by BLE client when device state changes."""
        if self._client:
            self.async_set_updated_data(self._client.state)

    def _schedule_reconnect(self) -> None:
        """Schedule a reconnection attempt."""
        if self._reconnect_task and not self._reconnect_task.done():
            return
        self._reconnect_task = self.hass.async_create_task(
            self._reconnect_loop()
        )

    async def _reconnect_loop(self) -> None:
        """Attempt reconnection with backoff."""
        delay = RECONNECT_DELAY
        while True:
            _LOGGER.debug("FJX7 %s: reconnecting in %ds", self.address, delay)
            await asyncio.sleep(delay)

            ble_device = bluetooth.async_ble_device_from_address(
                self.hass, self.address, connectable=True
            )
            if ble_device is None:
                _LOGGER.debug("FJX7 %s: not visible, retrying", self.address)
                delay = min(delay * 2, 120)
                continue

            try:
                await self._connect(ble_device)
                if self.is_connected:
                    _LOGGER.info("FJX7 %s: reconnected", self.address)
                    return
            except (BleakError, TimeoutError) as err:
                _LOGGER.debug("FJX7 %s: reconnect failed: %s", self.address, err)
                delay = min(delay * 2, 120)

    async def _async_update_data(self) -> FJX7State:
        """Called by DataUpdateCoordinator if polling were enabled."""
        return self.state
