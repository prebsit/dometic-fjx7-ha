"""Coordinator for Dometic FJX7 BLE integration.

Uses a polling model — connects, reads state, disconnects — rather
than holding a persistent BLE connection. This works around the Pi 3B+
bcm43438 BLE stability issues.
"""

from __future__ import annotations

import asyncio
import logging
from datetime import timedelta

from bleak.backends.device import BLEDevice

from homeassistant.components import bluetooth
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .client import FJX7BLEClient, FJX7State
from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

POLL_INTERVAL = timedelta(seconds=30)


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
            update_interval=POLL_INTERVAL,
        )
        self.address = address
        self._client: FJX7BLEClient | None = None

    @property
    def client(self) -> FJX7BLEClient | None:
        return self._client

    @property
    def state(self) -> FJX7State:
        if self._client:
            return self._client.state
        return FJX7State()

    @property
    def is_connected(self) -> bool:
        # Available once we've had at least one successful poll
        return self._client is not None and self._client.state.got_initial_state

    @property
    def has_polled(self) -> bool:
        return self._client is not None and self._client.state.got_initial_state

    async def async_start(self) -> None:
        """Start the coordinator — do first poll."""
        ble_device = bluetooth.async_ble_device_from_address(
            self.hass, self.address, connectable=True
        )
        if ble_device is None:
            _LOGGER.warning("FJX7 %s not found via Bluetooth", self.address)
            return

        self._client = FJX7BLEClient(
            ble_device,
            state_callback=self._on_state_changed,
        )

        # Do first poll — don't fail setup if it doesn't work yet
        try:
            success = await self._client.async_poll_state()
            if success:
                self.async_set_updated_data(self._client.state)
                _LOGGER.info("FJX7 %s: initial poll successful", self.address)
            else:
                _LOGGER.warning("FJX7 %s: initial poll failed, will retry in %ss", self.address, POLL_INTERVAL.seconds)
        except Exception as err:
            _LOGGER.warning("FJX7 %s: initial poll error: %s, will retry", self.address, err)

    async def async_stop(self) -> None:
        """Stop the coordinator."""
        self._client = None

    @callback
    def _on_state_changed(self) -> None:
        """Called by BLE client when device state changes."""
        if self._client:
            self.async_set_updated_data(self._client.state)

    async def _async_update_data(self) -> FJX7State:
        """Poll the FJX7 for current state."""
        if not self._client:
            raise UpdateFailed("No client")

        # Refresh the BLE device reference
        ble_device = bluetooth.async_ble_device_from_address(
            self.hass, self.address, connectable=True
        )
        if ble_device:
            self._client.set_ble_device(ble_device)

        success = await self._client.async_poll_state()
        if not success:
            raise UpdateFailed("Poll failed")

        return self._client.state
