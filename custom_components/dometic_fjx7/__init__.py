"""Dometic FreshJet FJX7 BLE integration for Home Assistant."""

from __future__ import annotations

import logging

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_ADDRESS, Platform
from homeassistant.core import HomeAssistant

from .const import DOMAIN
from .coordinator import FJX7Coordinator

_LOGGER = logging.getLogger(__name__)

PLATFORMS = [Platform.CLIMATE, Platform.LIGHT, Platform.SENSOR]

type FJX7ConfigEntry = ConfigEntry[FJX7Coordinator]


async def async_setup_entry(hass: HomeAssistant, entry: FJX7ConfigEntry) -> bool:
    """Set up Dometic FJX7 from a config entry."""
    address: str = entry.data[CONF_ADDRESS]

    coordinator = FJX7Coordinator(hass, address)
    await coordinator.async_start()

    entry.runtime_data = coordinator

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    entry.async_on_unload(coordinator.async_stop)

    return True


async def async_unload_entry(hass: HomeAssistant, entry: FJX7ConfigEntry) -> bool:
    """Unload a config entry."""
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
