"""Climate platform for Dometic FJX7."""

from __future__ import annotations

import logging
from typing import Any

from homeassistant.components.climate import (
    ClimateEntity,
    ClimateEntityFeature,
    HVACMode,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import ATTR_TEMPERATURE, UnitOfTemperature
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import (
    AC_MODE_AUTO,
    AC_MODE_COOL,
    AC_MODE_DRY,
    AC_MODE_FAN_ONLY,
    AC_MODE_HEAT,
    DOMAIN,
    FAN_SPEED_FROM_NAME,
    FAN_SPEED_NAMES,
    MANUFACTURER,
    TEMP_MAX,
    TEMP_MIN,
    TEMP_STEP,
)
from .coordinator import FJX7Coordinator

_LOGGER = logging.getLogger(__name__)

# Mapping: Dometic DDM mode → HA HVACMode
DDM_TO_HVAC: dict[int, HVACMode] = {
    AC_MODE_COOL: HVACMode.COOL,
    AC_MODE_HEAT: HVACMode.HEAT,
    AC_MODE_AUTO: HVACMode.AUTO,
    AC_MODE_DRY: HVACMode.DRY,
    AC_MODE_FAN_ONLY: HVACMode.FAN_ONLY,
}
HVAC_TO_DDM: dict[HVACMode, int] = {v: k for k, v in DDM_TO_HVAC.items()}


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up the FJX7 climate entity."""
    coordinator: FJX7Coordinator = entry.runtime_data
    async_add_entities([FJX7ClimateEntity(coordinator, entry)])


class FJX7ClimateEntity(CoordinatorEntity[FJX7Coordinator], ClimateEntity):
    """Climate entity for Dometic FJX7 roof aircon."""

    _attr_has_entity_name = True
    _attr_name = None  # Use device name
    _attr_temperature_unit = UnitOfTemperature.CELSIUS
    _attr_min_temp = TEMP_MIN
    _attr_max_temp = TEMP_MAX
    _attr_target_temperature_step = TEMP_STEP
    _attr_supported_features = (
        ClimateEntityFeature.TARGET_TEMPERATURE
        | ClimateEntityFeature.FAN_MODE
        | ClimateEntityFeature.TURN_ON
        | ClimateEntityFeature.TURN_OFF
    )
    _attr_hvac_modes = [
        HVACMode.OFF,
        HVACMode.COOL,
        HVACMode.HEAT,
        HVACMode.AUTO,
        HVACMode.DRY,
        HVACMode.FAN_ONLY,
    ]
    _attr_fan_modes = list(FAN_SPEED_NAMES.values())
    _enable_turn_on_off_backwards_compat = False

    def __init__(
        self, coordinator: FJX7Coordinator, entry: ConfigEntry
    ) -> None:
        """Initialise climate entity."""
        super().__init__(coordinator)
        self._attr_unique_id = f"{entry.unique_id}_climate"
        self._attr_device_info = {
            "identifiers": {(DOMAIN, coordinator.address)},
            "name": entry.title,
            "manufacturer": MANUFACTURER,
            "model": "FreshJet FJX7",
        }

    @property
    def available(self) -> bool:
        return self.coordinator.is_connected

    @property
    def hvac_mode(self) -> HVACMode:
        state = self.coordinator.state
        if not state.power:
            return HVACMode.OFF
        return DDM_TO_HVAC.get(state.ac_mode, HVACMode.AUTO)

    @property
    def current_temperature(self) -> float | None:
        return self.coordinator.state.measured_temp or None

    @property
    def target_temperature(self) -> float | None:
        return self.coordinator.state.target_temp

    @property
    def fan_mode(self) -> str | None:
        return FAN_SPEED_NAMES.get(self.coordinator.state.fan_speed)

    async def async_set_hvac_mode(self, hvac_mode: HVACMode) -> None:
        """Set HVAC mode."""
        client = self.coordinator.client
        if client is None:
            return

        if hvac_mode == HVACMode.OFF:
            await client.async_set_power(False)
        else:
            if not self.coordinator.state.power:
                await client.async_set_power(True)
            ddm_mode = HVAC_TO_DDM.get(hvac_mode)
            if ddm_mode is not None:
                await client.async_set_ac_mode(ddm_mode)

    async def async_set_temperature(self, **kwargs: Any) -> None:
        """Set target temperature."""
        client = self.coordinator.client
        temp = kwargs.get(ATTR_TEMPERATURE)
        if client is None or temp is None:
            return
        await client.async_set_temperature(temp)

    async def async_set_fan_mode(self, fan_mode: str) -> None:
        """Set fan speed."""
        client = self.coordinator.client
        if client is None:
            return
        speed = FAN_SPEED_FROM_NAME.get(fan_mode)
        if speed is not None:
            await client.async_set_fan_speed(speed)

    async def async_turn_on(self) -> None:
        """Turn on."""
        client = self.coordinator.client
        if client:
            await client.async_set_power(True)

    async def async_turn_off(self) -> None:
        """Turn off."""
        client = self.coordinator.client
        if client:
            await client.async_set_power(False)
