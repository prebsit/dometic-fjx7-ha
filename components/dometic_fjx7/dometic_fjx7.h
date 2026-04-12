#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/climate/climate_mode.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/sensor/sensor.h"
#include <vector>

namespace esphome {
namespace dometic_fjx7 {

using esp32_ble::ESPBTUUID;

// DDM Protocol constants
static const uint8_t DDM_CMD_REPORT = 0x10;
static const uint8_t DDM_CMD_SET = 0x11;
static const uint8_t DDM_CMD_SUBSCRIBE = 0x12;

static const uint8_t DDM_PARAM_POWER = 0x01;
static const uint8_t DDM_PARAM_FAN_SPEED = 0x02;
static const uint8_t DDM_PARAM_AC_MODE = 0x03;
static const uint8_t DDM_PARAM_TARGET_TEMP = 0x04;
static const uint8_t DDM_PARAM_INTERIOR_LIGHT = 0x05;
static const uint8_t DDM_PARAM_FAN_SPEED_PCT = 0x06;
static const uint8_t DDM_PARAM_MEASURED_TEMP = 0x0A;
static const uint8_t DDM_PARAM_EXTERIOR_LIGHT = 0x0E;

static const uint8_t DDM_GROUP_LO = 0x02;
static const uint8_t DDM_GROUP_HI = 0x01;

static const uint32_t AC_MODE_COOL = 0;
static const uint32_t AC_MODE_HEAT = 1;
static const uint32_t AC_MODE_FAN_ONLY = 2;
static const uint32_t AC_MODE_AUTO = 3;
static const uint32_t AC_MODE_DRY = 4;

static const uint32_t FAN_LOW = 0;
static const uint32_t FAN_MED = 1;
static const uint32_t FAN_HIGH = 2;
static const uint32_t FAN_TURBO = 3;
static const uint32_t FAN_AUTO = 5;

static const ESPBTUUID SERVICE_UUID =
    ESPBTUUID::from_raw("537a0400-0995-481f-926c-1604e23fd515");
static const ESPBTUUID WRITE_CHAR_UUID =
    ESPBTUUID::from_raw("537a0401-0995-481f-926c-1604e23fd515");
static const ESPBTUUID NOTIFY_CHAR_UUID =
    ESPBTUUID::from_raw("537a0402-0995-481f-926c-1604e23fd515");

class DometicFJX7Climate;
class DometicFJX7Light;

class DometicFJX7 : public ble_client::BLEClientNode, public Component {
 public:
  void setup() override;
  void loop() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_climate(DometicFJX7Climate *climate) { this->climate_ = climate; }
  void set_interior_light(DometicFJX7Light *light) { this->interior_light_ = light; }
  void set_exterior_light(DometicFJX7Light *light) { this->exterior_light_ = light; }
  void set_measured_temp_sensor(sensor::Sensor *sensor) { this->measured_temp_sensor_ = sensor; }
  void set_fan_speed_pct_sensor(sensor::Sensor *sensor) { this->fan_speed_pct_sensor_ = sensor; }

  void send_set_command(uint8_t param, uint32_t value);

 protected:
  void send_subscribe_(uint8_t param);
  void handle_report_(const uint8_t *data, uint16_t length);

  uint16_t write_handle_{0};
  uint16_t notify_handle_{0};
  bool subscribed_{false};

  bool power_{false};
  uint32_t fan_speed_{0};
  uint32_t ac_mode_{0};
  uint32_t target_temp_milli_{22000};
  uint32_t measured_temp_milli_{0};
  uint32_t fan_speed_pct_{0};
  bool interior_light_state_{false};
  bool exterior_light_state_{false};

  DometicFJX7Climate *climate_{nullptr};
  DometicFJX7Light *interior_light_{nullptr};
  DometicFJX7Light *exterior_light_{nullptr};
  sensor::Sensor *measured_temp_sensor_{nullptr};
  sensor::Sensor *fan_speed_pct_sensor_{nullptr};

  std::vector<uint8_t> subscribe_queue_;
  uint8_t subscribe_idx_{0};
  uint32_t last_subscribe_send_{0};
  uint32_t last_poll_{0};
};

class DometicFJX7Climate : public climate::Climate, public Component {
 public:
  void setup() override {}
  void set_parent(DometicFJX7 *parent) { this->parent_ = parent; }

  climate::ClimateTraits traits() override;
  void update_state(bool power, uint32_t ac_mode, uint32_t fan_speed,
                    float target_temp, float current_temp);

 protected:
  void control(const climate::ClimateCall &call) override;
  DometicFJX7 *parent_{nullptr};
};

class DometicFJX7Light : public light::LightOutput, public Component {
 public:
  void setup() override {}
  void set_parent(DometicFJX7 *parent) { this->parent_ = parent; }
  void set_param(uint8_t param) { this->param_ = param; }

  light::LightTraits get_traits() override;
  void write_state(light::LightState *state) override;
  void update_state(bool on);

 protected:
  DometicFJX7 *parent_{nullptr};
  uint8_t param_{0};
  light::LightState *light_state_{nullptr};
  uint32_t last_device_report_{0};
  bool last_device_state_{false};
};

}  // namespace dometic_fjx7
}  // namespace esphome
