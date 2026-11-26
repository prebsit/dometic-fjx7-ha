#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "history_logic.h"

#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#endif

#include <deque>
#include <string>
#include <vector>

namespace esphome {
namespace web_server {
class WebServer;
}  // namespace web_server
namespace ble_client {
class BLEClient;
}  // namespace ble_client
namespace climate {
class Climate;
class ClimateCall;
}  // namespace climate
namespace select {
class Select;
}  // namespace select
namespace button {
class Button;
}  // namespace button
namespace ice9_log {

class KeptLog;

// Who a command came from. Worked out from which part of ESPHome is running when the command
// lands (kept_log.cpp, door_now_()), and for the web, from the label the page and the Shortcut
// put on their requests.
enum class Door : uint8_t {
  ICE9,      // none of the doors below: the box's own doing (an automation in its config)
  PAGE,      // the ice9 page (X-Ice9-From: page)
  SHORTCUT,  // the iPhone Shortcut (X-Ice9-From: shortcut)
  WEB,       // a web request with no label: some other app on the Wi-Fi
  MQTT,      // a command topic on the broker: the GX's Node-RED on a Victron
  HA,        // the ESPHome API: Home Assistant
  AIRCON,    // a report from the aircon over Bluetooth: not a command at all
};

// What the aircon is set to, as far as the log cares (not the cabin temperature).
struct AcView {
  bool valid{false};
  uint8_t mode{0};  // climate::ClimateMode
  float target{0};
  uint8_t fan{0};   // FAN_* in kept_log.cpp
  bool sleep{false};
};

// When something was last asked for, to tell the aircon's answer from a change of its own. 0 is
// "never": a time that happens to be 0 is stored as 1.
struct Asked {
  uint32_t at{0};
  void now(uint32_t ms) { this->at = ms | 1; }
  bool recent(uint32_t ms, uint32_t window) const { return this->at != 0 && ms - this->at < window; }
};

#ifdef USE_LIGHT
// One per light: tells the log when the light changes, and from where.
class LightWatch : public light::LightRemoteValuesListener {
 public:
  LightWatch(KeptLog *log, light::LightState *light) : log_(log), light_(light) {}
  void on_light_remote_values_update() override;

  KeptLog *log_;
  light::LightState *light_;
  bool known_{false};
  bool on_{false};
  Asked asked_;
};
#endif

// The ice9's kept log (see __init__.py).
class KeptLog : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  // After the page password's gate (ice9_lock, BUS) and before the web server (WIFI - 1): the
  // observer must be asked about every request the gate lets through, ahead of the web server.
  float get_setup_priority() const override { return setup_priority::BUS - 1.0f; }

  void set_web_server(web_server::WebServer *web) { this->web_ = web; }
  void set_ble_client(ble_client::BLEClient *ble) { this->ble_ = ble; }
  void listen_to_logs();

  // GET /ice9/log: the last LOG_MAX lines since this start, "Booted: ..." first.
  std::string log_text();
  // GET /ice9/log/all: the history in flash, a piece at a time. `sink` returns false to stop.
  void history_stream(bool (*sink)(void *ctx, const char *data, size_t len), void *ctx);

  // From the web server's task, for every request the gate let through (WebDoor in kept_log.cpp).
  void web_request(bool post, bool update, const char *label, uint32_t ip);

#ifdef USE_LIGHT
  void light_changed_(LightWatch *w);
#endif

 protected:
  // Lines
  void log_line_(const std::string &e);
  bool history_begin_(bool *update_sent);
  void history_add_(const std::string &text, bool first = false);  // log_lock_ held
  void run_history_(uint32_t now);
  void history_flush_(size_t max_lines, bool even_while_read);
  std::string clock_str_() const;
  std::string mem_str_() const;

  // Who
  Door door_now_(std::string *from = nullptr);
  static const char *who_(Door d);
  static std::string asked_by_(Door d, const std::string &from, const std::string &carried, bool sent);

  // The aircon
  AcView ac_now_() const;
  static std::string ac_text_(const AcView &v);
  void on_climate_control_(climate::ClimateCall &call);
  void on_climate_state_();
  void finish_unasked_(uint32_t now);
  void on_power_limit_asked_(const char *value_text);  // in the log listener: no ESP_LOGx here
  void on_power_limit_state_(size_t index);
  void on_button_(button::Button *b);
  void on_restarting_();                              // in the log listener: no ESP_LOGx here
  bool aircon_linked_();
  void watch_aircon_link_(uint32_t now);

  // The network (as F451)
  void watch_network_(uint32_t now);
  void on_log_(uint8_t level, const char *tag, const char *message, size_t len);
  void wifi_line_(const std::string &text);
  void mqtt_line_(const std::string &text);

  web_server::WebServer *web_{nullptr};
  ble_client::BLEClient *ble_{nullptr};
  climate::Climate *ac_{nullptr};
  select::Select *power_limit_{nullptr};
  bool started_{false};  // the first loop() has run: everything before it is the box setting itself up

  // The log in memory, and the line pinned at its top.
  Mutex log_lock_;
  std::deque<std::string> log_;
  std::string boot_line_;
  int log_day_{-1};

  // The history in flash (history_logic.h). As F451: the web server's task reads it while the
  // main loop writes it, so hist_next_, hist_pending_, hist_readers_ and hist_skipped_ are only
  // touched with log_lock_ held.
  bool hist_ok_{false};
  bool hist_off_{false};
  volatile bool hist_has_pending_{false};
  uint32_t hist_nvs_{0};
  uint32_t hist_next_{0};
  std::deque<std::string> hist_pending_;
  uint32_t hist_waiting_since_ms_{0};
  uint8_t hist_readers_{0};
  uint16_t hist_skipped_{0};
  uint8_t hist_errors_{0};
  uint16_t hist_stack_left_{0};
  history::Limiter hist_limit_{120, 20000};  // 120 lines in a rush, then one every 20 s

  // The web door: written by the web server's task, read by the main loop. web_lock_ held.
  Mutex web_lock_;
  char web_label_[12]{};  // "page", "shortcut" or "" (no label)
  uint32_t web_ip_{0};    // the asker's IPv4 address, network order (0: not known)
  bool update_seen_{false};
  char update_label_[12]{};
  uint32_t update_ip_{0};
  volatile bool web_news_{false};

  // The aircon: what it was last reported to be, and what was asked of it lately.
  AcView ac_seen_;
  Asked ac_asked_[4];      // per setting (mode, temperature, fan, sleep)
  bool unasked_{false};    // a change nobody here asked for is settling
  AcView unasked_before_;
  uint32_t unasked_last_ms_{0};
  int power_limit_seen_{-1};  // option index last reported, -1 none yet
  Asked power_limit_asked_;
  bool link_up_{false};
  bool settling_{false};      // just connected: the reports are the baseline, not changes
  uint32_t settled_at_ms_{0};
  bool link_said_up_{false};  // "Connected to the aircon" has been said for this link
  AcView before_loss_;
  uint16_t link_tries_{0};
  uint32_t link_checked_ms_{0};

  // The network
  uint32_t net_checked_ms_{0};
  int8_t wifi_up_{-1};
  int8_t mqtt_up_{-1};
  int8_t portal_on_{-1};  // the setup hotspot's portal: -1 not looked at yet
  bool mqtt_was_up_{false};
  uint32_t mqtt_down_ms_{0};
  uint16_t wifi_fails_{0};
  uint16_t mqtt_fails_{0};
  std::vector<std::string> wifi_said_, mqtt_said_;

#ifdef USE_LIGHT
  std::vector<LightWatch *> lights_;
#endif
};

}  // namespace ice9_log
}  // namespace esphome
