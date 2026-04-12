#include "dometic_fjx7.h"
#include "esphome/core/log.h"
#include <esp_gap_ble_api.h>

namespace esphome {
namespace dometic_fjx7 {

static const char *const TAG = "dometic_fjx7";

void DometicFJX7::setup() {
  ESP_LOGI(TAG, "Dometic FJX7 component setup");
}

void DometicFJX7::dump_config() {
  ESP_LOGCONFIG(TAG, "Dometic FJX7:");
  ESP_LOGCONFIG(TAG, "  Write handle: 0x%04x", this->write_handle_);
  ESP_LOGCONFIG(TAG, "  Notify handle: 0x%04x", this->notify_handle_);
}

void DometicFJX7::loop() {
  // Stagger subscribe messages
  if (this->write_handle_ != 0 && !this->subscribed_ &&
      this->subscribe_idx_ < this->subscribe_queue_.size()) {
    uint32_t now = millis();
    if (now - this->last_subscribe_send_ > 200) {
      this->send_subscribe_(this->subscribe_queue_[this->subscribe_idx_]);
      this->subscribe_idx_++;
      this->last_subscribe_send_ = now;
      if (this->subscribe_idx_ >= this->subscribe_queue_.size()) {
        this->subscribed_ = true;
        ESP_LOGI(TAG, "All parameters subscribed");
      }
    }
  }

  // Periodic re-subscribe every 30s as safety net (notifications push changes instantly)
  if (this->subscribed_ && millis() - this->last_poll_ > 30000) {
    this->last_poll_ = millis();
    this->subscribed_ = false;
    this->subscribe_idx_ = 0;
    this->last_subscribe_send_ = millis();
    ESP_LOGD(TAG, "Polling \xe2\x80\x94 re-subscribing all params");
  }
}

void DometicFJX7::gattc_event_handler(esp_gattc_cb_event_t event,
                                       esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Connected \xe2\x80\x94 requesting encryption");
        // Force bonding/encryption immediately
        esp_ble_set_encryption(this->parent()->get_remote_bda(), ESP_BLE_SEC_ENCRYPT_MITM);
      } else {
        ESP_LOGW(TAG, "Connection failed, status=%d", param->open.status);
      }
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "Disconnected from FJX7");
      this->write_handle_ = 0;
      this->notify_handle_ = 0;
      this->subscribed_ = false;
      this->subscribe_idx_ = 0;
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *write_chr = this->parent()->get_characteristic(SERVICE_UUID, WRITE_CHAR_UUID);
      if (write_chr == nullptr) {
        ESP_LOGW(TAG, "Write characteristic not found");
        break;
      }
      this->write_handle_ = write_chr->handle;
      ESP_LOGI(TAG, "Write char handle: 0x%04x", this->write_handle_);

      auto *notify_chr = this->parent()->get_characteristic(SERVICE_UUID, NOTIFY_CHAR_UUID);
      if (notify_chr == nullptr) {
        ESP_LOGW(TAG, "Notify characteristic not found");
        break;
      }
      this->notify_handle_ = notify_chr->handle;
      ESP_LOGI(TAG, "Notify char handle: 0x%04x", this->notify_handle_);

      auto status = esp_ble_gattc_register_for_notify(
          this->parent()->get_gattc_if(), this->parent()->get_remote_bda(),
          this->notify_handle_);
      if (status != ESP_OK) {
        ESP_LOGW(TAG, "Register for notify failed: %d", status);
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Notification registration OK \xe2\x80\x94 starting subscribes");
        this->subscribe_queue_ = {
            DDM_PARAM_POWER, DDM_PARAM_FAN_SPEED, DDM_PARAM_AC_MODE,
            DDM_PARAM_TARGET_TEMP, DDM_PARAM_INTERIOR_LIGHT,
            DDM_PARAM_FAN_SPEED_PCT, DDM_PARAM_MEASURED_TEMP,
            DDM_PARAM_EXTERIOR_LIGHT};
        this->subscribe_idx_ = 0;
        this->subscribed_ = false;
        this->last_subscribe_send_ = millis();
      } else {
        ESP_LOGW(TAG, "Notification registration failed: %d", param->reg_for_notify.status);
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle == this->notify_handle_) {
        this->handle_report_(param->notify.value, param->notify.value_len);
      }
      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Write FAILED, handle=0x%04x status=%d",
                 param->write.handle, param->write.status);
      } else {
        ESP_LOGI(TAG, "Write OK, handle=0x%04x", param->write.handle);
      }
      break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT: {
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Descriptor write failed: %d", param->write.status);
      } else {
        ESP_LOGI(TAG, "CCCD written OK");
      }
      break;
    }

    default:
      break;
  }
}

void DometicFJX7::send_subscribe_(uint8_t param) {
  if (this->write_handle_ == 0) return;
  uint8_t data[5] = {DDM_CMD_SUBSCRIBE, param, 0x00, DDM_GROUP_LO, DDM_GROUP_HI};
  ESP_LOGD(TAG, "Subscribe param 0x%02x", param);
  esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
      this->write_handle_, sizeof(data), data,
      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

void DometicFJX7::send_set_command(uint8_t param, uint32_t value) {
  if (this->write_handle_ == 0) {
    ESP_LOGW(TAG, "Cannot send - not connected");
    return;
  }
  uint8_t data[9] = {
      DDM_CMD_SET, param, 0x00, DDM_GROUP_LO, DDM_GROUP_HI,
      (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF),
      (uint8_t)((value >> 16) & 0xFF), (uint8_t)((value >> 24) & 0xFF)};
  ESP_LOGI(TAG, "Set param 0x%02x = %u", param, value);
  esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
      this->write_handle_, sizeof(data), data,
      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

void DometicFJX7::handle_report_(const uint8_t *data, uint16_t length) {
  // Hex dump raw notification
  char hex[64] = {0};
  for (uint16_t i = 0; i < length && i < 20; i++) {
    sprintf(hex + i * 3, "%02x ", data[i]);
  }
  ESP_LOGI(TAG, "Notify raw [%d]: %s", length, hex);

  if (length < 9 || data[0] != DDM_CMD_REPORT) {
    ESP_LOGW(TAG, "Invalid report: len=%d cmd=0x%02x", length, length > 0 ? data[0] : 0);
    return;
  }
  uint8_t param = data[1];
  uint32_t value = data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24);
  ESP_LOGD(TAG, "Report: param=0x%02x value=%u", param, value);

  switch (param) {
    case DDM_PARAM_POWER:
      this->power_ = (value != 0);
      break;
    case DDM_PARAM_FAN_SPEED:
      this->fan_speed_ = value;
      break;
    case DDM_PARAM_AC_MODE:
      this->ac_mode_ = value;
      break;
    case DDM_PARAM_TARGET_TEMP:
      this->target_temp_milli_ = value;
      break;
    case DDM_PARAM_MEASURED_TEMP:
      this->measured_temp_milli_ = value;
      if (this->measured_temp_sensor_ != nullptr)
        this->measured_temp_sensor_->publish_state((float)value / 1000.0f);
      break;
    case DDM_PARAM_FAN_SPEED_PCT:
      this->fan_speed_pct_ = value;
      if (this->fan_speed_pct_sensor_ != nullptr)
        this->fan_speed_pct_sensor_->publish_state((float)value);
      break;
    case DDM_PARAM_INTERIOR_LIGHT:
      this->interior_light_state_ = (value != 0);
      if (this->interior_light_ != nullptr)
        this->interior_light_->update_state(this->interior_light_state_);
      break;
    case DDM_PARAM_EXTERIOR_LIGHT:
      this->exterior_light_state_ = (value != 0);
      if (this->exterior_light_ != nullptr)
        this->exterior_light_->update_state(this->exterior_light_state_);
      break;
    default:
      ESP_LOGD(TAG, "Unknown param 0x%02x = %u", param, value);
      break;
  }

  if (this->climate_ != nullptr &&
      (param == DDM_PARAM_POWER || param == DDM_PARAM_AC_MODE ||
       param == DDM_PARAM_FAN_SPEED || param == DDM_PARAM_TARGET_TEMP ||
       param == DDM_PARAM_MEASURED_TEMP)) {
    this->climate_->update_state(
        this->power_, this->ac_mode_, this->fan_speed_,
        (float)this->target_temp_milli_ / 1000.0f,
        (float)this->measured_temp_milli_ / 1000.0f);
  }
}

// ---- Climate ----

climate::ClimateTraits DometicFJX7Climate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_visual_min_temperature(16.0);
  traits.set_visual_max_temperature(31.0);
  traits.set_visual_temperature_step(1.0);
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_HEAT_COOL,
      climate::CLIMATE_MODE_FAN_ONLY,
      climate::CLIMATE_MODE_DRY,
  });
  traits.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_MEDIUM,
      climate::CLIMATE_FAN_HIGH,
  });
  traits.set_supported_custom_fan_modes({"Turbo"});
  return traits;
}

void DometicFJX7Climate::update_state(bool power, uint32_t ac_mode,
                                       uint32_t fan_speed, float target_temp,
                                       float current_temp) {
  if (!power) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch (ac_mode) {
      case AC_MODE_COOL: this->mode = climate::CLIMATE_MODE_COOL; break;
      case AC_MODE_HEAT: this->mode = climate::CLIMATE_MODE_HEAT; break;
      case AC_MODE_AUTO: this->mode = climate::CLIMATE_MODE_HEAT_COOL; break;
      case AC_MODE_FAN_ONLY: this->mode = climate::CLIMATE_MODE_FAN_ONLY; break;
      case AC_MODE_DRY: this->mode = climate::CLIMATE_MODE_DRY; break;
      default: this->mode = climate::CLIMATE_MODE_COOL; break;
    }
  }

  switch (fan_speed) {
    case FAN_AUTO:
      this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
      break;
    case FAN_LOW:
      this->set_fan_mode_(climate::CLIMATE_FAN_LOW);
      break;
    case FAN_MED:
      this->set_fan_mode_(climate::CLIMATE_FAN_MEDIUM);
      break;
    case FAN_HIGH:
      this->set_fan_mode_(climate::CLIMATE_FAN_HIGH);
      break;
    case FAN_TURBO:
      this->set_custom_fan_mode_("Turbo");
      break;
    default:
      this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
      break;
  }

  this->target_temperature = target_temp;
  this->current_temperature = current_temp;
  this->publish_state();
}

void DometicFJX7Climate::control(const climate::ClimateCall &call) {
  if (this->parent_ == nullptr) return;

  if (call.get_mode().has_value()) {
    auto mode = *call.get_mode();
    if (mode == climate::CLIMATE_MODE_OFF) {
      this->parent_->send_set_command(DDM_PARAM_POWER, 0);
    } else {
      this->parent_->send_set_command(DDM_PARAM_POWER, 1);
      uint32_t ac_mode = AC_MODE_COOL;
      switch (mode) {
        case climate::CLIMATE_MODE_COOL: ac_mode = AC_MODE_COOL; break;
        case climate::CLIMATE_MODE_HEAT: ac_mode = AC_MODE_HEAT; break;
        case climate::CLIMATE_MODE_HEAT_COOL: ac_mode = AC_MODE_AUTO; break;
        case climate::CLIMATE_MODE_FAN_ONLY: ac_mode = AC_MODE_FAN_ONLY; break;
        case climate::CLIMATE_MODE_DRY: ac_mode = AC_MODE_DRY; break;
        default: break;
      }
      this->parent_->send_set_command(DDM_PARAM_AC_MODE, ac_mode);
    }
  }

  if (call.get_fan_mode().has_value()) {
    uint32_t fan = FAN_AUTO;
    switch (*call.get_fan_mode()) {
      case climate::CLIMATE_FAN_AUTO: fan = FAN_AUTO; break;
      case climate::CLIMATE_FAN_LOW: fan = FAN_LOW; break;
      case climate::CLIMATE_FAN_MEDIUM: fan = FAN_MED; break;
      case climate::CLIMATE_FAN_HIGH: fan = FAN_HIGH; break;
      default: break;
    }
    this->parent_->send_set_command(DDM_PARAM_FAN_SPEED, fan);
  }

  if (call.has_custom_fan_mode()) {
    StringRef cfm = call.get_custom_fan_mode();
    if (cfm == "Turbo") {
      this->parent_->send_set_command(DDM_PARAM_FAN_SPEED, FAN_TURBO);
    }
  }

  if (call.get_target_temperature().has_value()) {
    uint32_t milli = (uint32_t)(*call.get_target_temperature() * 1000.0f);
    this->parent_->send_set_command(DDM_PARAM_TARGET_TEMP, milli);
  }
}

// ---- Light ----

light::LightTraits DometicFJX7Light::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::ON_OFF});
  return traits;
}

void DometicFJX7Light::write_state(light::LightState *state) {
  this->light_state_ = state;
  if (this->parent_ == nullptr) return;
  bool on;
  state->current_values_as_binary(&on);
  // Suppress echo: if this write was triggered by a device report (within 500ms)
  // and the state matches what the device just told us, skip the DDM write
  if (millis() - this->last_device_report_ < 500 && on == this->last_device_state_) {
    ESP_LOGD("dometic_fjx7", "Light write_state suppressed (echo from device report)");
    return;
  }
  this->parent_->send_set_command(this->param_, on ? 1 : 0);
}

void DometicFJX7Light::update_state(bool on) {
  // Record what the device told us \xe2\x80\x94 write_state() checks this to suppress echoes
  this->last_device_report_ = millis();
  this->last_device_state_ = on;
  if (this->light_state_ != nullptr) {
    auto call = on ? this->light_state_->turn_on() : this->light_state_->turn_off();
    call.perform();
  }
}

}  // namespace dometic_fjx7
}  // namespace esphome
