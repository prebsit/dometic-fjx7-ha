#include "kept_log.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/time.h"
#include "esphome/core/version.h"

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/dometic_fjx7/dometic_fjx7.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/web_server/web_server.h"
#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <nvs.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace esphome {
namespace ice9_log {

// Not "ice9...": the log keeps what tags starting "ice9" say (the page password's lines), and
// must not keep its own.
static const char *const TAG = "kept_log";

static const size_t LOG_MAX = 100;           // lines kept for GET /ice9/log
static const uint32_t HIST_MAX = 1200;       // lines kept in flash; after that the oldest is overwritten
static const size_t HIST_TEXT_MAX = 140;     // a longer line is cut: at worst 1200 x 192 B = 230 kB of flash
static const uint32_t HIST_LAYOUT = (1UL << 16) | HIST_MAX;  // stored with it: any other number starts it afresh
static const size_t HIST_PENDING_MAX = 100;  // lines waiting to be written
static const uint32_t HIST_WAIT_MS = 2000;   // a line waits at most this long for three more to go with it
static const char *const HIST_NAMESPACE = "ice9log";
static const time_t CLOCK_SET = 1700000000;  // the clock reads later than this once it has been set

// A change the aircon reports within this long of a command for the same thing is its answer.
static const uint32_t ASKED_MS = 8000;
// A change nobody here asked for is written once the aircon has been still for this long, so that
// one press of its remote, which it reports a setting at a time, is one line.
static const uint32_t SETTLE_MS = 2000;
// After the Bluetooth link comes up, the aircon tells the box everything, a setting at a time:
// that is where it starts from, not changes. Long enough for the whole round (dometic_fjx7 asks
// for each setting 200 ms apart, once the link is encrypted and the notifications are on).
static const uint32_t LINK_SETTLE_MS = 10000;

static const char NOT_ASKED[] = ". Not asked by the ice9: its own panel, remote or app";

enum : uint8_t { F_MODE = 0, F_TEMP, F_FAN, F_SLEEP };
static constexpr uint8_t FAN_AUTO = 0, FAN_LOW = 1, FAN_MEDIUM = 2, FAN_HIGH = 3, FAN_TURBO = 4, FAN_OTHER = 5;

static const char *reset_reason_text() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "power on";
    case ESP_RST_EXT:
      return "reset pin";
    case ESP_RST_SW:
      return "restart";
    case ESP_RST_PANIC:
      return "CRASH (panic)";
    case ESP_RST_INT_WDT:
      return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT:
      return "CRASH (task watchdog)";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep sleep";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT: the supply dipped";
    case ESP_RST_USB:
      return "reset over USB";  // a computer on the cable: flashing a box over USB ends with one
    default:
      return "other";
  }
}

// An entity's name (EntityBase's own getter is deprecated from ESPHome 2026.11 and goes in 2027.5).
static std::string entity_name(const EntityBase *e) {
  char buf[ENTITY_NAME_BUF_SIZE];
  const size_t n = e->write_name_to(buf, sizeof(buf));
  return std::string(buf, n < sizeof(buf) ? n : sizeof(buf) - 1);
}

static std::string ip_text(uint32_t ip) {
  if (ip == 0)
    return "";
  uint8_t b[4];
  memcpy(b, &ip, 4);  // network order: the first byte is the first number
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return buf;
}

// ---- The web: who is asking, and the log itself ------------------------------

// The asker's IPv4 address (network order), or 0. A phone on IPv4 reaches an IPv6 socket as
// ::ffff:a.b.c.d.
static uint32_t peer_ipv4(httpd_req_t *req) {
  const int fd = httpd_req_to_sockfd(req);
  if (fd < 0)
    return 0;
  struct sockaddr_storage sa {};
  socklen_t len = sizeof(sa);
  if (getpeername(fd, (struct sockaddr *) &sa, &len) != 0)
    return 0;
  if (sa.ss_family == AF_INET)
    return ((struct sockaddr_in *) &sa)->sin_addr.s_addr;
#if LWIP_IPV6
  if (sa.ss_family == AF_INET6) {
    const uint8_t *b = (const uint8_t *) &((struct sockaddr_in6 *) &sa)->sin6_addr;
    static const uint8_t MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (memcmp(b, MAPPED, 12) == 0) {
      uint32_t v;
      memcpy(&v, b + 12, 4);
      return v;
    }
  }
#endif
  return 0;
}

// Asked about every request the page password's gate lets through, ahead of the web server, and
// never takes one: it only notes, for a POST, who is asking. The web server carries out a command
// on the main loop a moment later (DEFER_ACTION in web_server.cpp), and the log looks there.
// The page and the Shortcut say who they are: a request header "X-Ice9-From: page" or "shortcut".
class WebDoor : public AsyncWebHandler {
 public:
  explicit WebDoor(KeptLog *parent) : parent_(parent) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() == HTTP_POST)
      note(this->parent_, request);
    return false;
  }
  void handleRequest(AsyncWebServerRequest *request) override {}

 protected:
  // A function of its own, never inlined: the URL buffer is half a kilobyte, off the web server
  // task's small stack again before the next handler runs.
  static void __attribute__((noinline)) note(KeptLog *parent, AsyncWebServerRequest *request) {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const bool update = request->url_to(url_buf) == "/update";
    char label[12] = "";
    auto from = request->get_header("X-Ice9-From");
    if (from.has_value()) {
      std::string v = *from;
      for (char &c : v)
        c = (char) tolower((unsigned char) c);
      if (v == "page" || v == "shortcut")
        strncpy(label, v.c_str(), sizeof(label) - 1);
    }
    parent->web_request(true, update, label, peer_ipv4(*request));
  }
  KeptLog *parent_;
};

// GET /ice9/log      since the ice9 last started: the last LOG_MAX lines as plain text, oldest first.
// GET /ice9/log/all  the history kept in flash: about a week, through restarts and power cuts.
// Word for word F451's (GET /f451/log), so the page reads both the same way.
class LogHandler : public AsyncWebHandler {
 public:
  explicit LogHandler(KeptLog *parent) : parent_(parent) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET)
      return false;
    return which(request) != 0;
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    if (which(request) == 2) {
      // Sent a piece at a time (chunked), straight from flash: as one string it could be 100 kB,
      // more memory in one block than the board may have to spare.
      httpd_req_t *req = *request;
      Sending s{req, true};
      httpd_resp_set_status(s.req, HTTPD_200);
      httpd_resp_set_type(s.req, "text/plain; charset=utf-8");
      httpd_resp_set_hdr(s.req, "Cache-Control", "no-store");
      this->parent_->history_stream(&LogHandler::send_piece, &s);
      if (s.ok) {
        httpd_resp_send_chunk(s.req, nullptr, 0);  // the end
      } else {
        // The reader went away half way through: drop the connection rather than leave it half sent.
        httpd_sess_trigger_close(s.req->handle, httpd_req_to_sockfd(s.req));
      }
      return;
    }
    std::string body = this->parent_->log_text();
    request->send(200, "text/plain; charset=utf-8", body.c_str());
  }

 protected:
  struct Sending {
    httpd_req_t *req;
    bool ok;
  };
  static bool send_piece(void *ctx, const char *data, size_t len) {
    auto *s = static_cast<Sending *>(ctx);
    if (len > 0 && httpd_resp_send_chunk(s->req, data, (ssize_t) len) != ESP_OK)
      s->ok = false;
    return s->ok;
  }
  // 0 = not ours, 1 = /ice9/log, 2 = /ice9/log/all. Never inlined: see WebDoor.
  static int __attribute__((noinline)) which(AsyncWebServerRequest *request) {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const auto url = request->url_to(url_buf);
    if (url == "/ice9/log")
      return 1;
    if (url == "/ice9/log/all")
      return 2;
    return 0;
  }
  KeptLog *parent_;
};

void KeptLog::web_request(bool post, bool update, const char *label, uint32_t ip) {
  if (!post)
    return;
  LockGuard guard(this->web_lock_);
  strncpy(this->web_label_, label, sizeof(this->web_label_) - 1);
  this->web_label_[sizeof(this->web_label_) - 1] = '\0';
  this->web_ip_ = ip;
  if (update) {
    this->update_seen_ = true;
    memcpy(this->update_label_, this->web_label_, sizeof(this->update_label_));
    this->update_ip_ = ip;
    this->web_news_ = true;
  }
}

// ---- Who asked ------------------------------------------------------------------

// Which door the command now landing came in by. ESPHome runs one thing at a time on its main
// loop and keeps note of which (Application::get_current_component(), set for every component's
// loop() and every scheduled callback): a command from Home Assistant is carried out inside the
// API server's loop, one from MQTT inside the MQTT client's, one from the web inside a callback
// the web server scheduled, and a report from the aircon inside the Bluetooth stack's loop.
Door KeptLog::door_now_(std::string *from) {
  Component *c = App.get_current_component();
  if (c == nullptr)
    return Door::ICE9;
  if (esp32_ble::global_ble != nullptr && c == esp32_ble::global_ble)
    return Door::AIRCON;
#ifdef USE_MQTT
  if (mqtt::global_mqtt_client != nullptr && c == mqtt::global_mqtt_client)
    return Door::MQTT;
#endif
#ifdef USE_API
  if (api::global_api_server != nullptr && c == api::global_api_server)
    return Door::HA;
#endif
  if (this->web_ != nullptr && c == this->web_) {
    LockGuard guard(this->web_lock_);
    if (from != nullptr)
      *from = ip_text(this->web_ip_);
    if (strcmp(this->web_label_, "page") == 0)
      return Door::PAGE;
    if (strcmp(this->web_label_, "shortcut") == 0)
      return Door::SHORTCUT;
    return Door::WEB;
  }
  return Door::ICE9;
}

const char *KeptLog::who_(Door d) {
  switch (d) {
    case Door::PAGE:
      return "the page";
    case Door::SHORTCUT:
      return "the Shortcut";
    case Door::WEB:
      return "another app on the Wi-Fi";
    case Door::MQTT:
      return "MQTT (the GX or Node-RED)";
    case Door::HA:
      return "Home Assistant";
    case Door::AIRCON:
      return "the aircon";
    default:
      return "the ice9 itself";
  }
}

// ". Asked by: the page~ (mode, temperature; from 192.168.1.23)"
std::string KeptLog::asked_by_(Door d, const std::string &from, const std::string &carried, bool sent) {
  std::string s = ". Asked by: ";
  s += who_(d);
  if (!sent)
    s += ". Not sent: the aircon isn't connected";
  if (!carried.empty() || !from.empty()) {
    s += "~ (";
    s += carried;
    if (!from.empty()) {
      if (!carried.empty())
        s += "; ";
      s += "from " + from;
    }
    s += ")";
  }
  return s;
}

// ---- The aircon ------------------------------------------------------------------

#ifdef USE_CLIMATE
static uint8_t fan_code(climate::ClimateFanMode f) {
  switch (f) {
    case climate::CLIMATE_FAN_AUTO:
      return FAN_AUTO;
    case climate::CLIMATE_FAN_LOW:
      return FAN_LOW;
    case climate::CLIMATE_FAN_MEDIUM:
      return FAN_MEDIUM;
    case climate::CLIMATE_FAN_HIGH:
      return FAN_HIGH;
    default:
      return FAN_OTHER;
  }
}
#endif

AcView KeptLog::ac_now_() const {
  AcView v;
#ifdef USE_CLIMATE
  climate::Climate *c = this->ac_;
  if (c == nullptr)
    return v;
  v.valid = true;
  v.mode = (uint8_t) c->mode;
  v.target = c->target_temperature;
  if (c->has_custom_fan_mode()) {
    v.fan = c->get_custom_fan_mode() == "Turbo" ? FAN_TURBO : FAN_OTHER;
  } else {
    v.fan = c->fan_mode.has_value() ? fan_code(*c->fan_mode) : FAN_AUTO;
  }
  v.sleep = c->preset.has_value() && *c->preset == climate::CLIMATE_PRESET_SLEEP;
#endif
  return v;
}

// Which of the settings differ (bit F_MODE etc.).
static uint8_t ac_diff(const AcView &a, const AcView &b) {
  uint8_t d = 0;
  if (a.mode != b.mode)
    d |= 1 << F_MODE;
  if (std::isnan(a.target) != std::isnan(b.target) || (!std::isnan(a.target) && fabsf(a.target - b.target) > 0.05f))
    d |= 1 << F_TEMP;
  if (a.fan != b.fan)
    d |= 1 << F_FAN;
  if (a.sleep != b.sleep)
    d |= 1 << F_SLEEP;
  return d;
}

// The page's words (page/ice9.js, MODES and FANS).
static const char *mode_word(uint8_t m) {
#ifdef USE_CLIMATE
  switch ((climate::ClimateMode) m) {
    case climate::CLIMATE_MODE_OFF:
      return "Off";
    case climate::CLIMATE_MODE_COOL:
      return "Cool";
    case climate::CLIMATE_MODE_HEAT:
      return "Heat";
    case climate::CLIMATE_MODE_HEAT_COOL:
    case climate::CLIMATE_MODE_AUTO:
      return "Auto";
    case climate::CLIMATE_MODE_FAN_ONLY:
      return "Fan";
    case climate::CLIMATE_MODE_DRY:
      return "Dry";
    default:
      break;
  }
#endif
  return "an unknown mode";
}

static const char *fan_word(uint8_t f) {
  static const char *const WORDS[] = {"Auto", "Low", "Medium", "High", "Turbo", "?"};
  return WORDS[f <= FAN_OTHER ? f : FAN_OTHER];
}

// "21°, fan Auto" (the settings that go with a mode)
static std::string settings_text(const AcView &v, bool with_temp) {
  std::string s;
  if (with_temp && !std::isnan(v.target)) {
    char t[12];
    const float r = roundf(v.target);
    if (fabsf(v.target - r) < 0.05f) {
      snprintf(t, sizeof(t), "%d°", (int) r);
    } else {
      snprintf(t, sizeof(t), "%.1f°", v.target);
    }
    s += t;
    s += ", ";
  }
  s += "fan ";
  s += fan_word(v.fan);
  if (v.sleep)
    s += ", Sleep";
  return s;
}

// "Cool, 21°, fan Auto", "Fan, fan High", "Off"
std::string KeptLog::ac_text_(const AcView &v) {
#ifdef USE_CLIMATE
  if (v.mode == (uint8_t) climate::CLIMATE_MODE_OFF)
    return "Off";
  const bool temp = v.mode != (uint8_t) climate::CLIMATE_MODE_FAN_ONLY;
#else
  const bool temp = true;
#endif
  return std::string(mode_word(v.mode)) + ", " + settings_text(v, temp);
}

bool KeptLog::aircon_linked_() { return this->ble_ != nullptr && this->ble_->connected(); }

#ifdef USE_CLIMATE
// A command for the aircon (from anywhere): ESPHome calls this before it is carried out
// (ClimateCall::perform), with what was asked. The line says what the aircon is being set to.
void KeptLog::on_climate_control_(climate::ClimateCall &call) {
  if (!this->started_)
    return;
  std::string from;
  const Door d = this->door_now_(&from);
  const AcView was = this->ac_now_();
  AcView v = was;
  std::string carried;
  const uint32_t now = millis();
  auto add = [&](const char *what, uint8_t field) {
    if (!carried.empty())
      carried += ", ";
    carried += what;
    this->ac_asked_[field].now(now);
  };
  if (call.get_mode().has_value()) {
    v.mode = (uint8_t) *call.get_mode();
    add("mode", F_MODE);
  }
  if (call.get_target_temperature().has_value()) {
    v.target = *call.get_target_temperature();
    add("temperature", F_TEMP);
  }
  if (call.has_custom_fan_mode()) {
    v.fan = call.get_custom_fan_mode() == "Turbo" ? FAN_TURBO : FAN_OTHER;
    add("fan", F_FAN);
  } else if (call.get_fan_mode().has_value()) {
    v.fan = fan_code(*call.get_fan_mode());
    add("fan", F_FAN);
  }
  if (call.get_preset().has_value()) {
    v.sleep = *call.get_preset() == climate::CLIMATE_PRESET_SLEEP;
    add("sleep", F_SLEEP);
  }
  if (carried.empty())
    return;
  std::string what;
  if (v.mode == (uint8_t) climate::CLIMATE_MODE_OFF && !call.get_mode().has_value()) {
    what = "While off: " + settings_text(v, true);  // a setting changed, and it stays off
  } else {
    what = ac_text_(v);
  }
  this->log_line_(what + this->asked_by_(d, from, carried, this->aircon_linked_()));
}

// The aircon's state as last reported (dometic_fjx7 publishes only when something has changed:
// after a command, after a press of its own panel or remote, and the cabin temperature).
void KeptLog::on_climate_state_() {
  const AcView now_v = this->ac_now_();
  const AcView was = this->ac_seen_;
  this->ac_seen_ = now_v;
  if (!this->started_ || this->settling_ || !was.valid)
    return;
  const uint8_t changed = ac_diff(was, now_v);
  if (changed == 0)
    return;  // the cabin temperature
  const uint32_t now = millis();
  uint8_t theirs = 0;
  for (uint8_t f = 0; f < 4; f++) {
    if ((changed & (1 << f)) && !this->ac_asked_[f].recent(now, ASKED_MS))
      theirs |= 1 << f;
  }
  if (theirs == 0)
    return;  // the aircon doing what it was asked
  if (!this->unasked_) {
    this->unasked_ = true;
    this->unasked_before_ = was;
  }
  this->unasked_last_ms_ = now;
}
#endif

// A change nobody here asked for, once the aircon has been still for SETTLE_MS: one line.
void KeptLog::finish_unasked_(uint32_t now) {
  if (!this->unasked_ || now - this->unasked_last_ms_ < SETTLE_MS)
    return;
  this->unasked_ = false;
#ifdef USE_CLIMATE
  const AcView &b = this->unasked_before_, &a = this->ac_seen_;
  if (ac_diff(b, a) == 0)
    return;  // and back again
  const uint8_t off = (uint8_t) climate::CLIMATE_MODE_OFF;
  std::string what;
  if (b.mode == off && a.mode != off) {
    what = "The aircon turned on: " + ac_text_(a);
  } else if (b.mode != off && a.mode == off) {
    what = "The aircon turned off";
  } else if (a.mode == off) {
    what = "The aircon's settings changed while it was off: " + settings_text(a, true);
  } else {
    what = "The aircon changed to " + ac_text_(a);
  }
  this->log_line_(what + NOT_ASKED);
#endif
}

#ifdef USE_LIGHT
void LightWatch::on_light_remote_values_update() { this->log_->light_changed_(this); }

// The page's words for the two lights (page/ice9.js); any other light by its own name.
static std::string light_word(light::LightState *l) {
  const std::string name = entity_name(l);
  if (name == "Interior Light")
    return "Inside light";
  if (name == "Exterior Light")
    return "Outside light";
  return name;
}

// A light changed: a command (from a door), or the aircon reporting one (dometic_fjx7 mirrors its
// lights with a light call of its own, from the Bluetooth stack's loop).
void KeptLog::light_changed_(LightWatch *w) {
  const bool on = w->light_->remote_values.is_on();
  if (!this->started_ || !w->known_) {
    w->known_ = true;
    w->on_ = on;
    return;
  }
  if (on == w->on_)
    return;
  w->on_ = on;
  std::string from;
  const Door d = this->door_now_(&from);
  const uint32_t now = millis();
  const std::string what = light_word(w->light_) + (on ? " on" : " off");
  if (d == Door::AIRCON) {
    if (this->settling_ || w->asked_.recent(now, ASKED_MS))
      return;  // where it starts from, or the answer to a command
    this->log_line_(what + NOT_ASKED);
    return;
  }
  w->asked_.now(now);
  this->log_line_(what + this->asked_by_(d, from, "", this->aircon_linked_()));
}
#endif

// "Power limit 5 A", "No power limit" (the select's options are 4A, 5A, 6A, 7A, Unlimited).
static std::string power_limit_text(const char *option) {
  if (option == nullptr)
    return "Power limit: unknown";
  const size_t n = strlen(option);
  if (n >= 2 && option[n - 1] == 'A' && option[n - 2] >= '0' && option[n - 2] <= '9')
    return "Power limit " + std::string(option, n - 1) + " A";
  if (strcmp(option, "Unlimited") == 0)
    return "No power limit";
  return std::string("Power limit ") + option;
}

// The FJZ's power limit has been sent to the aircon. ESPHome's select has no hook for a command,
// so this comes from dometic_fjx7's own line, "Set param 0x2d = 1", which it writes as it sends
// it, still inside the command's door. Called from the log listener: nothing here may log.
void KeptLog::on_power_limit_asked_(const char *value_text) {
#ifdef USE_SELECT
  if (!this->started_ || this->power_limit_ == nullptr)
    return;
  const unsigned long raw = strtoul(value_text, nullptr, 10);
  const char *option = nullptr;
  for (size_t i = 0; i < dometic_fjx7::ADAPTIVE_POWER_COUNT; i++) {
    if (dometic_fjx7::ADAPTIVE_POWER_VALUES[i] == raw)
      option = this->power_limit_->option_at(i);
  }
  std::string from;
  const Door d = this->door_now_(&from);
  this->power_limit_asked_.now(millis());
  this->log_line_(power_limit_text(option) + this->asked_by_(d, from, "", true));
#endif
}

void KeptLog::on_power_limit_state_(size_t index) {
#ifdef USE_SELECT
  const int was = this->power_limit_seen_;
  this->power_limit_seen_ = (int) index;
  if (!this->started_ || this->settling_ || was < 0 || was == (int) index)
    return;
  if (this->power_limit_asked_.recent(millis(), ASKED_MS))
    return;
  this->log_line_(power_limit_text(this->power_limit_->option_at(index)) + NOT_ASKED);
#endif
}

// A restart on its way (on_log_). Who asked, when it came in by a door: the page's Restart, the
// MQTT settings saved; otherwise the box's own doing (an update, Forget AC's pause, a script).
void KeptLog::on_restarting_() {
  std::string from;
  const Door d = this->door_now_(&from);
  std::string line = "Restarting";
  if (d != Door::ICE9 && d != Door::AIRCON)
    line += this->asked_by_(d, from, "", true);
  this->log_line_(line);
}

#ifdef USE_BUTTON
void KeptLog::on_button_(button::Button *b) {
  if (!this->started_)
    return;
  std::string from;
  const Door d = this->door_now_(&from);
  const std::string name = entity_name(b);
  this->log_line_(name + " pressed" + this->asked_by_(d, from, "", true));
}
#endif

// The Bluetooth link to the aircon, looked at twice a second.
void KeptLog::watch_aircon_link_(uint32_t now) {
  if (now - this->link_checked_ms_ < 500)
    return;
  this->link_checked_ms_ = now;
  const bool up = this->aircon_linked_();
  if (up && !this->link_up_) {
    this->link_up_ = true;
    this->settling_ = true;
    this->settled_at_ms_ = now + LINK_SETTLE_MS;
  } else if (!up && this->link_up_) {
    this->link_up_ = false;
    if (this->settling_) {
      this->settling_ = false;  // gone again before it had settled: a try that didn't take
      if (this->link_tries_ < 0xFFFF)
        this->link_tries_++;
    } else if (this->link_said_up_) {
      this->link_said_up_ = false;
      this->before_loss_ = this->ac_seen_;
      this->unasked_ = false;
      this->log_line_("Lost the Bluetooth link to the aircon~" + this->mem_str_());
    }
  }
  if (this->settling_ && (int32_t) (now - this->settled_at_ms_) >= 0) {
    this->settling_ = false;
    this->link_said_up_ = true;
    this->unasked_ = false;
    std::string line = "Connected to the aircon";
    if (this->ac_seen_.valid)
      line += ": " + ac_text_(this->ac_seen_);
    if (this->before_loss_.valid && this->ac_seen_.valid && ac_diff(this->before_loss_, this->ac_seen_) != 0)
      line += std::string(". It changed while the link was down") + NOT_ASKED;
    if (this->link_tries_ > 0) {
      char buf[48];
      snprintf(buf, sizeof(buf), "~ (after %u tr%s that didn't hold)", (unsigned) this->link_tries_,
               this->link_tries_ == 1 ? "y" : "ies");
      line += buf;
    }
    this->link_tries_ = 0;
    this->log_line_(line);
  }
}

// ---- Setup and loop ---------------------------------------------------------------

void KeptLog::setup() {
  bool update_sent = false;
  const bool new_firmware = this->history_begin_(&update_sent);
  {
    // Pinned: the log's first line however much happens after it (log_text()). It carries the
    // date when the clock knows it (after a restart it does; after a power cut, not yet): the
    // lines under it have the time only, and a heading when the day changes.
#ifdef ESPHOME_PROJECT_VERSION
    static const char VERSION[] = ESPHOME_PROJECT_VERSION;
#else
    static const char VERSION[] = "";
#endif
    char built[Application::BUILD_TIME_STR_SIZE];
    App.get_build_time_string(built);
    std::string text = std::string("Booted: ") + reset_reason_text();
    if (new_firmware) {
      text += ", new firmware";
    } else if (update_sent) {
      // A file went to POST /update before the last start, and the same firmware is running: it was
      // refused, the upload broke off, or the new firmware wouldn't start and the box went back to
      // the one it had (a factory image sent as an update does that, and the page says "Successful").
      text += ", but the update didn't take: still the same firmware";
    }
    text += std::string("~ (ice9 ") + VERSION + ", built " + built + ", ESPHome " + ESPHOME_VERSION + ")";
    std::string boot;
    const time_t t = ::time(nullptr);
    if (t > CLOCK_SET) {
      struct tm lt {};
      ::localtime_r(&t, &lt);
      char when[32];
      strftime(when, sizeof(when), "%a %d %b %H:%M:%S", &lt);
      boot = std::string(when) + " " + text;
      this->log_day_ = history::day_key(lt);
    } else {
      boot = this->clock_str_() + " " + text;
    }
    LockGuard guard(this->log_lock_);
    this->boot_line_ = std::move(boot);
    this->history_add_(text, true);
  }

  // After the page password's gate (ice9_lock adds it at an earlier priority), before the web server.
  web_server_base::global_web_server_base->add_handler(new WebDoor(this));     // NOLINT
  web_server_base::global_web_server_base->add_handler(new LogHandler(this));  // NOLINT

#ifdef USE_CLIMATE
  for (auto *c : App.get_climates()) {
    this->ac_ = c;  // the one aircon
    break;
  }
  if (this->ac_ != nullptr) {
    this->ac_->add_on_control_callback([this](climate::ClimateCall &call) { this->on_climate_control_(call); });
    this->ac_->add_on_state_callback([this](climate::Climate &) { this->on_climate_state_(); });
  }
#endif
#ifdef USE_LIGHT
  for (auto *l : App.get_lights()) {
    auto *w = new LightWatch(this, l);  // NOLINT
    l->add_remote_values_listener(w);
    this->lights_.push_back(w);
  }
#endif
#ifdef USE_SELECT
  // The FJZ's power limit (packages/adaptive_power.yaml), when the build has it. By name: entity
  // names never change once boxes have shipped.
  for (auto *s : App.get_selects()) {
    if (entity_name(s) == "Adaptive Power") {
      this->power_limit_ = s;
      s->add_on_state_callback([this](size_t index) { this->on_power_limit_state_(index); });
    }
  }
#endif
#ifdef USE_BUTTON
  for (auto *b : App.get_buttons())
    b->add_on_press_callback([this, b]() { this->on_button_(b); });
#endif
}

void KeptLog::loop() {
  const uint32_t now = millis();
  this->started_ = true;
  if (this->web_news_) {
    bool update = false;
    char label[12];
    uint32_t ip = 0;
    {
      LockGuard guard(this->web_lock_);
      this->web_news_ = false;
      update = this->update_seen_;
      this->update_seen_ = false;
      memcpy(label, this->update_label_, sizeof(label));
      ip = this->update_ip_;
    }
    if (update) {
      const Door d = strcmp(label, "page") == 0       ? Door::PAGE
                     : strcmp(label, "shortcut") == 0 ? Door::SHORTCUT
                                                      : Door::WEB;
      this->log_line_("Firmware update: a file is being sent" + this->asked_by_(d, ip_text(ip), "", true));
      // Noted in flash at once, for the boot line after it (setup()).
      if (this->hist_ok_ && nvs_set_u8(this->hist_nvs_, "upd", 1) == ESP_OK)
        nvs_commit(this->hist_nvs_);
    }
  }
  this->watch_network_(now);
  this->watch_aircon_link_(now);
  this->finish_unasked_(now);
  this->run_history_(now);
}

void KeptLog::dump_config() {
  ESP_LOGCONFIG(TAG, "ice9 kept log:");
  ESP_LOGCONFIG(TAG, "  GET /ice9/log (last %u lines)", (unsigned) LOG_MAX);
  if (this->hist_ok_) {
    ESP_LOGCONFIG(TAG, "  GET /ice9/log/all: history in flash (%u lines kept, room for %u)",
                  (unsigned) (this->hist_next_ < HIST_MAX ? this->hist_next_ : HIST_MAX), (unsigned) HIST_MAX);
  } else {
    ESP_LOGCONFIG(TAG, "  History in flash: not available (memory only)");
  }
  if (this->hist_stack_left_ != 0)
    ESP_LOGCONFIG(TAG, "  Web server task's spare stack at the last history read: %u bytes",
                  (unsigned) this->hist_stack_left_);
  ESP_LOGCONFIG(TAG, "  Aircon: %s%s%s", this->ac_ != nullptr ? "climate" : "no climate",
                this->power_limit_ != nullptr ? ", power limit" : "", this->ble_ != nullptr ? ", Bluetooth link" : "");
}

void KeptLog::on_shutdown() {
  if (this->hist_ok_)
    this->history_flush_(HIST_PENDING_MAX + 2, true);
}

// ---- The log --------------------------------------------------------------------

// Local time once the clock is set (SNTP or Home Assistant), else time since the start.
std::string KeptLog::clock_str_() const {
  char buf[16];
  const time_t t = ::time(nullptr);
  if (t > CLOCK_SET) {
    ESPTime lt = ESPTime::from_epoch_local(t);
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (unsigned) lt.hour, (unsigned) lt.minute, (unsigned) lt.second);
  } else {
    const uint32_t s = millis() / 1000;
    snprintf(buf, sizeof(buf), "+%u:%02u", (unsigned) (s / 60), (unsigned) (s % 60));
  }
  return buf;
}

// A line for the log and the history. '~' marks where what only support needs begins (where a
// command came from, network reasons, memory): the page shows the line up to it unless "Show the
// detail" is on, and a line that starts with '~' is detail all through. Copy and Share keep it all.
// Nothing else may contain a '~' (on_log_ swaps the layer's for '-').
void KeptLog::log_line_(const std::string &e) {
  std::string line = this->clock_str_() + " " + e;
  const time_t t = ::time(nullptr);
  LockGuard guard(this->log_lock_);
  // The lines carry the time only, so the day goes in as a heading whenever it changes.
  if (t > CLOCK_SET) {
    struct tm lt {};
    ::localtime_r(&t, &lt);
    const int day = history::day_key(lt);
    if (day != this->log_day_) {
      this->log_day_ = day;
      this->log_.push_back(history::day_marker(lt));
    }
  }
  this->log_.push_back(std::move(line));
  while (this->log_.size() > LOG_MAX)
    this->log_.pop_front();
  this->history_add_(e);
}

std::string KeptLog::log_text() {
  LockGuard guard(this->log_lock_);
  std::string out;
  out.reserve(this->log_.size() * 64 + 96);
  if (!this->boot_line_.empty()) {
    out += this->boot_line_;
    out += '\n';
  }
  for (const auto &l : this->log_) {
    out += l;
    out += '\n';
  }
  return out;
}

// Free internal RAM now, its lowest since the start and the largest block left, for a drop's
// line: whether the ice9 was short of memory when the link went.
std::string KeptLog::mem_str_() const {
  char buf[96];
  snprintf(buf, sizeof(buf), " (memory: %u KB free, %u KB at its lowest, largest block %u KB)",
           (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned) (heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
  return buf;
}

// ---- The history: the log, kept in flash ------------------------------------------
// The layout, the day headings and the limit on a rush of lines are in history_logic.h (F451's,
// tested on a computer: tests/history_logic_test.cpp). Here: NVS. Unlike F451 there is no bus to
// keep quiet for: a line waits only for a few more to go with it (HIST_WAIT_MS).

// True if this firmware isn't the one that ran last time (by when it was built). `update_sent`: a
// file went to POST /update before this start (the note is cleared as it is read).
bool KeptLog::history_begin_(bool *update_sent) {
  nvs_handle_t h = 0;
  esp_err_t err = nvs_open(HIST_NAMESPACE, NVS_READWRITE, &h);
  uint32_t layout = 0, next = 0;
  if (err == ESP_OK && (nvs_get_u32(h, "layout", &layout) != ESP_OK || layout != HIST_LAYOUT ||
                        nvs_get_u32(h, "next", &next) != ESP_OK)) {
    // The first start with a history, or one kept in another layout: begin afresh.
    next = 0;
    err = nvs_erase_all(h);
    if (err == ESP_OK)
      err = nvs_set_u32(h, "layout", HIST_LAYOUT);
    if (err == ESP_OK)
      err = nvs_set_u32(h, "next", 0);
    if (err == ESP_OK)
      err = nvs_commit(h);
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "History: flash wouldn't open (%s): the log is in memory only", esp_err_to_name(err));
    if (h != 0)
      nvs_close(h);
    LockGuard guard(this->log_lock_);
    this->hist_off_ = true;
    this->hist_pending_.clear();
    this->hist_has_pending_ = false;
    return false;
  }
  // The firmware: a different build time from last start's is new firmware. None stored (the
  // first start with this log, or after a factory reset): nothing to say.
  bool fresh = false;
  const uint32_t built = (uint32_t) App.get_build_time();
  uint32_t was = 0;
  const esp_err_t got = nvs_get_u32(h, "built", &was);
  if (got != ESP_OK || was != built) {
    fresh = got == ESP_OK;
    if (nvs_set_u32(h, "built", built) == ESP_OK)
      nvs_commit(h);
  }
  uint8_t upd = 0;
  if (nvs_get_u8(h, "upd", &upd) == ESP_OK) {
    *update_sent = upd != 0;
    if (nvs_erase_key(h, "upd") == ESP_OK)
      nvs_commit(h);
  }
  LockGuard guard(this->log_lock_);
  this->hist_nvs_ = h;
  this->hist_next_ = next;
  this->hist_ok_ = true;
  return fresh;
}

// log_lock_ is held.
void KeptLog::history_add_(const std::string &text, bool first) {
  if (this->hist_off_)
    return;
  const uint32_t ms = millis();
  // A rush of lines (a link flapping all night) must not push the rest of the week out: past
  // the limit they stay in memory only, and the next line kept says how many were left out.
  if (!first && !this->hist_limit_.take(ms)) {
    if (this->hist_skipped_ < 0xFFFF)
      this->hist_skipped_++;
    return;
  }
  const time_t t = ::time(nullptr);
  const bool clock = t > CLOCK_SET;
  const uint32_t stamp = clock ? (uint32_t) t : ms / 1000;
  if (this->hist_pending_.empty())
    this->hist_waiting_since_ms_ = ms;
  if (first) {
    this->hist_pending_.push_front(history::make_record(clock, stamp, text, HIST_TEXT_MAX));
  } else {
    if (this->hist_skipped_ > 0) {
      char buf[72];
      snprintf(buf, sizeof(buf), "(%u more lines were not kept here: too many at once)", (unsigned) this->hist_skipped_);
      this->hist_skipped_ = 0;
      this->hist_pending_.push_back(history::make_record(clock, stamp, buf, HIST_TEXT_MAX));
    }
    this->hist_pending_.push_back(history::make_record(clock, stamp, text, HIST_TEXT_MAX));
  }
  // Flash not taking them (or not open yet, or being read for a long time): the oldest go.
  while (this->hist_pending_.size() > HIST_PENDING_MAX) {
    this->hist_pending_.pop_front();
    if (this->hist_skipped_ < 0xFFFF)
      this->hist_skipped_++;
  }
  this->hist_has_pending_ = true;
}

void KeptLog::run_history_(uint32_t now) {
  if (!this->hist_ok_ || !this->hist_has_pending_)
    return;
  {
    LockGuard guard(this->log_lock_);
    if (this->hist_pending_.size() < 4 && now - this->hist_waiting_since_ms_ < HIST_WAIT_MS)
      return;
  }
  this->history_flush_(4, false);
}

void KeptLog::history_flush_(size_t max_lines, bool even_while_read) {
  esp_err_t err = ESP_OK;
  bool full = false, stopped = false;
  {
    // Held across the writes, so that someone reading the history sees it either before a line
    // went in or after, never half way.
    LockGuard guard(this->log_lock_);
    size_t done = 0;
    while (done < max_lines && !this->hist_pending_.empty() && (even_while_read || this->hist_readers_ == 0)) {
      char key[12];
      history::slot_key(key, history::slot_of(this->hist_next_, HIST_MAX));
      err = nvs_set_str(this->hist_nvs_, key, this->hist_pending_.front().c_str());
      if (err != ESP_OK)
        break;
      this->hist_next_++;
      this->hist_pending_.pop_front();
      done++;
    }
    if (done > 0) {
      esp_err_t e = nvs_set_u32(this->hist_nvs_, "next", this->hist_next_);
      if (e == ESP_OK)
        e = nvs_commit(this->hist_nvs_);
      if (err == ESP_OK)
        err = e;
    }
    if (err == ESP_OK) {
      this->hist_errors_ = 0;
    } else if (++this->hist_errors_ >= 3) {
      this->hist_errors_ = 0;
      if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        // It shouldn't happen (the history is sized to half the partition at worst), but the
        // settings live in the same flash and matter more: give the space back and begin again.
        full = true;
        this->hist_pending_.clear();
        this->hist_next_ = 0;
        if (nvs_erase_all(this->hist_nvs_) != ESP_OK || nvs_set_u32(this->hist_nvs_, "layout", HIST_LAYOUT) != ESP_OK ||
            nvs_set_u32(this->hist_nvs_, "next", 0) != ESP_OK || nvs_commit(this->hist_nvs_) != ESP_OK)
          stopped = true;
      } else {
        stopped = true;
      }
      if (stopped) {
        this->hist_ok_ = false;
        this->hist_off_ = true;
        this->hist_pending_.clear();
      }
    }
    this->hist_has_pending_ = !this->hist_pending_.empty();
  }
  if (err != ESP_OK)
    ESP_LOGW(TAG, "History: writing to flash failed (%s)", esp_err_to_name(err));
  // Said in the log itself, outside the lock (log_line_ takes it).
  if (full)
    this->log_line_("History cleared: the flash it shares with the settings was full");
  if (stopped)
    this->log_line_("History: flash has stopped taking lines, so the log is in memory only until the next start");
}

void KeptLog::history_stream(bool (*sink)(void *ctx, const char *data, size_t len), void *ctx) {
  // Everything here is on the heap or written out by hand: this runs in the web server's task,
  // which has about 4 kB of stack, and printf alone can take a quarter of that.
  uint32_t next = 0;
  std::vector<std::string> waiting;
  bool ok;
  {
    LockGuard guard(this->log_lock_);
    ok = this->hist_ok_;
    if (ok) {
      this->hist_readers_++;  // nothing is written until we're done: the oldest lines can't change under us
      next = this->hist_next_;
      waiting.assign(this->hist_pending_.begin(), this->hist_pending_.end());
    }
  }
  if (!ok) {
    static const char NONE[] =
        "History: not available. The ice9's flash wouldn't take it, so the log is in memory only: /ice9/log\n";
    sink(ctx, NONE, sizeof(NONE) - 1);
    return;
  }
  const uint32_t first = history::first_kept(next, HIST_MAX);
  std::string out;
  out.reserve(2048);
  out += "History: ";
  history::put_number(out, next - first + (uint32_t) waiting.size());
  out += " lines, oldest first (room for ";
  history::put_number(out, HIST_MAX);
  out += "; then the oldest go)\n";
  std::string rec(history::STAMP_LEN + HIST_TEXT_MAX + 8, '\0');
  int day = -1;
  bool going = true;
  for (uint32_t n = first; going && n != next; n++) {
    char key[12];
    history::slot_key(key, history::slot_of(n, HIST_MAX));
    size_t len = rec.size();
    if (nvs_get_str(this->hist_nvs_, key, &rec[0], &len) == ESP_OK)
      history::render_record(rec.c_str(), day, out);
    if (out.size() >= 1500) {
      going = sink(ctx, out.data(), out.size());
      out.clear();
    }
    if ((n & 31) == 31)
      vTaskDelay(1);  // a thousand flash reads in a row: let the main loop in between
  }
  // The newest few may still be waiting to be written.
  for (const auto &w : waiting)
    history::render_record(w.c_str(), day, out);
  if (going && !out.empty())
    sink(ctx, out.data(), out.size());
  // How close that came to the end of this task's stack, for dump_config (no logging from here:
  // formatting a log line is itself the biggest user of it).
  const UBaseType_t spare = uxTaskGetStackHighWaterMark(nullptr);
  this->hist_stack_left_ = spare > 0xFFFF ? 0xFFFF : (uint16_t) spare;
  LockGuard guard(this->log_lock_);
  if (this->hist_readers_ > 0)
    this->hist_readers_--;
}

// ---- What the rest of the box logs, and the network (as F451) -------------------------

void KeptLog::listen_to_logs() {
#if defined(USE_LOGGER) && defined(USE_LOG_LISTENERS)
  if (logger::global_logger != nullptr)
    logger::global_logger->add_log_callback(
        this, [](void *self, uint8_t level, const char *tag, const char *message, size_t len) {
          static_cast<KeptLog *>(self)->on_log_(level, tag, message, len);
        });
#endif
}

// Called inside ESP_LOGx: must not log. Kept, INFO and worse: the page password (tags ice9.*),
// pairing (pair), the broker setting (mqtt_cfg), resets (reset) and the packages' own logger.log
// lines (main); the Wi-Fi's and MQTT's own complaints, in our words; and from the aircon's
// component only the line that says a power limit has gone to the aircon.
void KeptLog::on_log_(uint8_t level, const char *tag, const char *message, size_t len) {
  if (level > ESPHOME_LOG_LEVEL_INFO || level == ESPHOME_LOG_LEVEL_NONE || tag == nullptr || message == nullptr)
    return;
  const bool wifi = strcmp(tag, "wifi") == 0 || strcmp(tag, "wifi_esp32") == 0;
  const bool mqtt = strcmp(tag, "mqtt") == 0;
  const bool ac = strcmp(tag, "dometic_fjx7") == 0;
  const bool app = strcmp(tag, "app") == 0;
  const bool layer = strncmp(tag, "ice9", 4) == 0 || strcmp(tag, "pair") == 0 || strcmp(tag, "mqtt_cfg") == 0 ||
                     strcmp(tag, "reset") == 0 || strcmp(tag, "main") == 0;
  if (!wifi && !mqtt && !ac && !app && !layer)
    return;
  // The line comes formatted: "\033[0;32m[I][pair:157]: Paired with ...\033[0m". Drop the colour
  // codes, then everything up to the first "]: ". What ESPHome marks secret (LOG_SECRET: between
  // "\033[8m" and "\033[28m") is left out altogether.
  std::string text;
  text.reserve(len);
  bool secret = false;
  for (size_t i = 0; i < len && message[i] != '\0'; i++) {
    if (message[i] != '\033') {
      if (!secret)
        text += message[i];
      continue;
    }
    size_t j = i;
    while (j < len && message[j] != 'm')
      j++;
    if (j - i == 3 && i + 2 < len && message[i + 2] == '8') {
      secret = true;
    } else if (j - i == 4 && i + 3 < len && message[i + 2] == '2' && message[i + 3] == '8') {
      secret = false;
    }
    i = j;
  }
  const size_t at = text.find("]: ");
  if (at == std::string::npos)
    return;
  text.erase(0, at + 3);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  if (text.empty())
    return;
  if (app) {
    // Application::safe_reboot(): every restart that isn't a crash or a power cut says this first,
    // still inside whatever asked for it (the Restart button reboots inside its own press, before
    // any press hook of ours could run). on_shutdown() then writes the line to flash.
    if (text == "Rebooting safely")
      this->on_restarting_();
    return;
  }
  if (ac) {
    // dometic_fjx7::send_set_command: "Set param 0x2d = 1", logged as it goes to the aircon.
    static_assert(dometic_fjx7::DDM_PARAM_ADAPTIVE_POWER == 0x2D, "the power limit's parameter");
    static const char SENT[] = "Set param 0x2d = ";
    if (text.compare(0, sizeof(SENT) - 1, SENT) == 0)
      this->on_power_limit_asked_(text.c_str() + sizeof(SENT) - 1);
    return;
  }
  if (wifi) {
    this->wifi_line_(text);
    return;
  }
  if (mqtt) {
    this->mqtt_line_(text);
    return;
  }
  // reset.yaml counts towards the five-power-cycle factory reset at every start: "Power cycle 0
  // of 5" after a restart (seen on the desk board, 26 Nov 2026), and presumably 1 after an ordinary
  // plug-in. Read by an owner as half way to a wipe. Two or more in a row are news: someone doing
  // the reset, or a supply that keeps dropping out.
  if (strcmp(tag, "main") == 0 && text.rfind("Power cycle ", 0) == 0 &&
      (text.compare(12, 2, "0 ") == 0 || text.compare(12, 2, "1 ") == 0))
    return;
  for (char &c : text) {
    if (c == '~')
      c = '-';
  }
  if (text.size() > 160) {
    text.resize(157);
    text += "...";
  }
  this->log_line_(level == ESPHOME_LOG_LEVEL_ERROR  ? "Error: " + text
                  : level == ESPHOME_LOG_LEVEL_WARN ? "Warning: " + text
                                                    : text);
}

void KeptLog::wifi_line_(const std::string &text) {
  std::string line;
  if (text.rfind("Disconnected", 0) == 0) {
    // "Disconnected ssid='...' bssid=... reason='4-Way Handshake Timeout'"
    std::string reason = "no reason given";
    const size_t at = text.find("reason='");
    if (at != std::string::npos) {
      const size_t end = text.find('\'', at + 8);
      reason = text.substr(at + 8, end == std::string::npos ? std::string::npos : end - (at + 8));
    }
    if (this->wifi_up_ == 1) {
      // A working link has gone. The driver says so before is_connected() changes, so this
      // is the drop itself: say why, with the memory, and the poll mustn't say it again.
      this->wifi_up_ = 0;
      this->wifi_said_.clear();
      this->log_line_("Wi-Fi dropped~: " + reason + this->mem_str_());
      return;
    }
    this->wifi_fails_++;
    // "Authentication Failed" and "Handshake Failed" don't always mean a wrong password: F451
    // saw both all night on 24 Nov 2026 with the right one, on a weak link.
    // "Auth Expired" and "Association Expired" are the access point not answering in time: a weak
    // link, not the password (the desk board at -79 dBm, 26 Nov 2026, joined on the third try).
    const bool expired = reason.find("Expired") != std::string::npos;
    const bool password = !expired && (reason.find("Handshake") != std::string::npos || reason.find("Auth") != std::string::npos ||
                                       reason.find("MIC") != std::string::npos || reason.find("Mic") != std::string::npos);
    const bool missing = reason.find("Probe Request") != std::string::npos || reason.find("Not Found") != std::string::npos;
    // Something the owner can fix is shown; anything else is detail (the page's "Show the detail").
    line = password ? "Wi-Fi: couldn't join: turned away at the password step (a wrong password, or a weak link)~ (" +
                          reason + ")"
           : missing ? "Wi-Fi: couldn't join: the network wasn't found (out of range, off, or renamed)~ (" + reason + ")"
                     : "~Wi-Fi: couldn't join: " + reason;
  } else {
    return;
  }
  // A retry loop says the same things every few seconds: once each is enough.
  for (const auto &said : this->wifi_said_) {
    if (said == line)
      return;
  }
  if (this->wifi_said_.size() >= 6)
    return;
  this->wifi_said_.push_back(line);
  this->log_line_(line);
}

// ESPHome's MQTT lines, in our words (F451, 24 Nov 2026: the GX refusing the login said nothing
// in the log, and a broker that stopped answering looked like nothing).
void KeptLog::mqtt_line_(const std::string &text) {
  std::string line;
  if (text.rfind("Disconnected: ", 0) == 0) {
    const std::string reason = text.substr(14);
    if (this->mqtt_was_up_) {
      this->mqtt_was_up_ = false;
      this->mqtt_down_ms_ = 0;
      this->mqtt_said_.clear();
      this->log_line_("MQTT disconnected~: " + reason + this->mem_str_());
      return;
    }
    this->mqtt_fails_++;
    line = "~MQTT: couldn't connect: " + reason;
  } else if (text.rfind("Connection refused error: ", 0) == 0) {
    // The broker answered and said no. A Victron GX says 5 (not authorised) to a wrong password,
    // to no login on Weak or Secured, and to a client ID it remembers from before a login was set.
    static const char *const WHY[] = {"", "unsupported MQTT version", "client ID rejected", "broker unavailable",
                                      "bad user name or password", "not authorised"};
    const unsigned long code = strtoul(text.c_str() + 26, nullptr, 16);
    char buf[128];
    snprintf(buf, sizeof(buf), "MQTT: the broker refused the ice9 (%s)%s~ (code %lu)",
             code >= 1 && code <= 5 ? WHY[code] : "unknown reason",
             code == 4 || code == 5 ? ": check the MQTT user name and password in Settings" : "", code);
    line = buf;
  } else if (text.rfind("Last esp-tls error", 0) == 0) {
    // "... socket errno: 113 (Software caused connection abort)": the broker didn't answer at all
    const size_t at = text.find("socket errno: ");
    if (at == std::string::npos || text.compare(at + 14, 2, "0 ") == 0)
      return;
    line = "MQTT: couldn't reach the broker~: socket error " + text.substr(at + 14);
  } else {
    return;
  }
  for (const auto &said : this->mqtt_said_) {
    if (said == line)
      return;
  }
  if (this->mqtt_said_.size() >= 6)
    return;
  this->mqtt_said_.push_back(line);
  this->log_line_(line);
}

// MQTT and Wi-Fi coming and going, and the setup hotspot.
void KeptLog::watch_network_(uint32_t now) {
  if (now - this->net_checked_ms_ < 2000)
    return;
  this->net_checked_ms_ = now;
#ifdef USE_CAPTIVE_PORTAL
  // The setup hotspot, by its portal: ESPHome says nothing it would be safe to catch when a box with
  // no Wi-Fi saved starts it (seen on a newly flashed box, 26 Nov 2026).
  if (captive_portal::global_captive_portal != nullptr) {
    const int8_t on = captive_portal::global_captive_portal->is_active() ? 1 : 0;
    if (on != this->portal_on_) {
      if (on == 1) {
        this->log_line_("Setup hotspot " + std::string(App.get_name().c_str()) + " on, waiting to be given a Wi-Fi");
      } else if (this->portal_on_ == 1) {
        this->log_line_("Setup hotspot off");
      }
      this->portal_on_ = on;
    }
  }
#endif
#ifdef USE_MQTT
  if (mqtt::global_mqtt_client != nullptr) {
    int8_t up = mqtt::global_mqtt_client->is_connected() ? 1 : 0;
    if (up == 0 && this->mqtt_was_up_) {
      // ESPHome says why a moment later ("Disconnected: ..."), and mqtt_line_ logs that with the
      // reason. This is for when it doesn't.
      if (this->mqtt_down_ms_ == 0) {
        this->mqtt_down_ms_ = now;
      } else if (now - this->mqtt_down_ms_ >= 4000) {
        this->mqtt_was_up_ = false;
        this->mqtt_down_ms_ = 0;
        this->log_line_("MQTT disconnected~" + this->mem_str_());
      }
    }
    if (up == 1 && this->mqtt_up_ != 1) {
      if (this->mqtt_was_up_ && this->mqtt_down_ms_ != 0)
        this->log_line_("MQTT disconnected~, briefly");  // down and back between two looks
      char buf[80];
      int n = snprintf(buf, sizeof(buf), "MQTT connected~");
      if (this->mqtt_fails_ > 0 && n > 0 && (size_t) n < sizeof(buf))
        snprintf(buf + n, sizeof(buf) - n, ", after %u failed attempt%s", (unsigned) this->mqtt_fails_,
                 this->mqtt_fails_ == 1 ? "" : "s");
      this->log_line_(buf);
      this->mqtt_fails_ = 0;
      this->mqtt_said_.clear();
      this->mqtt_was_up_ = true;
      this->mqtt_down_ms_ = 0;
    }
    this->mqtt_up_ = up;
  }
#endif
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    int8_t up = wifi::global_wifi_component->is_connected() ? 1 : 0;
    if (up != this->wifi_up_) {
      if (up == 1) {
        char buf[80];
        int n = snprintf(buf, sizeof(buf), "Wi-Fi connected~ (%d dBm)", (int) wifi::global_wifi_component->wifi_rssi());
        if (this->wifi_fails_ > 0 && n > 0 && (size_t) n < sizeof(buf))
          snprintf(buf + n, sizeof(buf) - n, ", after %u failed attempt%s", (unsigned) this->wifi_fails_,
                   this->wifi_fails_ == 1 ? "" : "s");
        this->log_line_(buf);
        this->wifi_fails_ = 0;
        this->wifi_said_.clear();
      } else if (this->wifi_up_ != -1) {
        this->log_line_("Wi-Fi dropped~" + this->mem_str_());  // no reason from the driver: see wifi_line_
      }
      this->wifi_up_ = up;
    }
  }
#endif
}

}  // namespace ice9_log
}  // namespace esphome
