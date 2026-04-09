"""Light platform for Dometic FJX7 (interior and exterior lights)."""

from __future__ import annotations

from homeassistant.components.light import ColorMode, LightEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN, MANUFACTURER, PARAM_EXTERIOR_LIGHT, PARAM_INTERIOR_LIGHT
from .coordinator import FJX7Coordinator


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up FJX7 light entities."""
    coordinator: FJX7Coordinator = entry.runtime_data
    async_add_entities([
        FJX7Light(coordinator, entry, PARAM_INTERIOR_LIGHT, "Interior Light"),
        FJX7Light(coordinator, entry, PARAM_EXTERIOR_LIGHT, "Exterior Light"),
    ])


class FJX7Light(CoordinatorEntity[FJX7Coordinator], LightEntity):
    """On/off light entity for FJX7 interior or exterior light."""

    _attr_has_entity_name = True
    _attr_color_mode = ColorMode.ONOFF
    _attr_supported_color_modes = {ColorMode.ONOFF}

    def __init__(
        self,
        coordinator: FJX7Coordinator,
        entry: ConfigEntry,
        param_id: int,
        name: str,
    ) -> None:
        super().__init__(coordinator)
        self._param_id = param_id
        self._attr_name = name
        self._attr_unique_id = f"{entry.unique_id}_light_{param_id:#04x}"
        self._attr_device_info = {
            "identifiers": {(DOMAIN, coordinator.address)},
        }

    @property
    def available(self) -> bool:
        return self.coordinator.is_connected

    @property
    def is_on(self) -> bool:
        if self._param_id == PARAM_INTERIOR_LIGHT:
            return self.coordinator.state.interior_light
        return self.coordinator.state.exterior_light

    async def async_turn_on(self, **kwargs) -> None:
        client = self.coordinator.client
        if client:
            await client.async_set_light(self._param_id, True)

    async def async_turn_off(self, **kwargs) -> None:
        client = self.coordinator.client
        if client:
            await client.async_set_light(self._param_id, False)
