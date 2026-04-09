"""Sensor platform for Dometic FJX7."""

from __future__ import annotations

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import PERCENTAGE, UnitOfTemperature
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN, MANUFACTURER
from .coordinator import FJX7Coordinator


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up FJX7 sensor entities."""
    coordinator: FJX7Coordinator = entry.runtime_data
    async_add_entities([
        FJX7TemperatureSensor(coordinator, entry),
        FJX7FanSpeedSensor(coordinator, entry),
    ])


class FJX7TemperatureSensor(CoordinatorEntity[FJX7Coordinator], SensorEntity):
    """Measured interior temperature sensor."""

    _attr_has_entity_name = True
    _attr_name = "Measured Temperature"
    _attr_device_class = SensorDeviceClass.TEMPERATURE
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = UnitOfTemperature.CELSIUS
    _attr_suggested_display_precision = 1

    def __init__(self, coordinator: FJX7Coordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{entry.unique_id}_measured_temp"
        self._attr_device_info = {
            "identifiers": {(DOMAIN, coordinator.address)},
        }

    @property
    def available(self) -> bool:
        return self.coordinator.is_connected

    @property
    def native_value(self) -> float | None:
        val = self.coordinator.state.measured_temp
        return val if val > 0 else None


class FJX7FanSpeedSensor(CoordinatorEntity[FJX7Coordinator], SensorEntity):
    """Fan speed percentage sensor (read-only feedback)."""

    _attr_has_entity_name = True
    _attr_name = "Fan Speed"
    _attr_native_unit_of_measurement = PERCENTAGE
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_icon = "mdi:fan"

    def __init__(self, coordinator: FJX7Coordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{entry.unique_id}_fan_speed_pct"
        self._attr_device_info = {
            "identifiers": {(DOMAIN, coordinator.address)},
        }

    @property
    def available(self) -> bool:
        return self.coordinator.is_connected

    @property
    def native_value(self) -> int | None:
        return self.coordinator.state.fan_speed_pct
