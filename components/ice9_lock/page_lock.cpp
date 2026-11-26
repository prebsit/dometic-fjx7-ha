#include "page_lock.h"

#include "esphome/core/application.h"
// get_mac_address() moved here during 2026.10; helpers.h stopped pulling it in with 2026.11.0.
#if __has_include("esphome/core/alloc_helpers.h")
#include "esphome/core/alloc_helpers.h"
#endif
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/version.h"
#include "esphome/core/log.h"
#include "esphome/components/web_server_base/web_server_base.h"

#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif
// The ice9 setup screen (SetupScreen, below) needs the setup hotspot's portal, the web server
// that serves the page, and a page of our own on it: a build with ESPHome's stock interface has
// no setup screen to show, and keeps ESPHome's setup page.
#if defined(USE_CAPTIVE_PORTAL) && defined(USE_WEBSERVER) && defined(USE_WEBSERVER_JS_INCLUDE)
#define ICE9_SETUP_SCREEN
#include "esphome/components/web_server/web_server.h"
#include "esphome/components/wifi/wifi_component.h"
#include <lwip/ip_addr.h>
#endif

#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>

namespace esphome {
namespace ice9_lock {

// When a password was set, kept, removed or went back goes in the log at INFO.
static const char *const TAG = "ice9.lock";
static const uint32_t MAGIC = 0x4C4F4B31;  // "LOK1": the stored layout
static const size_t MIN_LEN = 4;

// The refusal. It starts "No permission" so the iPhone shortcut has words to look for; the rest
// is what a person sees if they cancel the login box.
static const char REFUSED[] =
    "No permission: this ice9 has a page password.\n\n"
    "Reload this page and type it in. The user name can be anything (or nothing): only the password counts.\n\n"
    "Forgotten it? It can only be cleared with your hands on the ice9: see the setup guide, \"Starting again\".\n";

// A firmware file sent with no page password set and the plug-in window shut.
static const char NO_UPLOAD[] =
    "No permission: unplug the ice9, plug it back in, and send the update again within 10 minutes.\n\n"
    "An ice9 with no page password only takes new firmware just after it has been plugged in.\n"
    "With a page password set (Settings) it takes it at any time, from someone who has given it.\n";

static bool printable(const std::string &s) {
  for (char c : s) {
    if (c < 0x20 || c > 0x7E)
      return false;
  }
  return true;
}

// Equal, taking the same time however early they differ.
static bool same(const char *a, const char *b, size_t lb) {
  const size_t la = strlen(a);
  uint8_t diff = la == lb ? 0 : 1;
  for (size_t k = 0; k < lb; k++)
    diff |= (uint8_t) (b[k] ^ (k < la ? a[k] : 0));
  return diff == 0;
}

// The first handler the web server asks about every request (added first, without ESPHome's own
// login check, which this replaces): it takes the request, and refuses it, unless the request
// carries the password. ESPHome's check wants the user name to match too, and nobody would know
// what to type (F451, 24 Nov 2026: they'd lock themselves out at once).
//
// It is also what makes the firmware upload (POST /update, ESPHome's web_server OTA) safe to
// leave on. With a page password set, the upload works for someone who has given it. With none
// set (an open ice9 is the owner's choice, 25 Nov 2026) it works for 10 minutes after the box
// is plugged in, and is refused here the rest of the time: new firmware takes either the password
// or hands on the box. The setup hotspot is always open: getting on needs the card's password.
class Gate : public AsyncWebHandler {
 public:
  explicit Gate(PageLock *parent) : parent_(parent) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (!this->admitted_(request))
      return true;
    return this->upload_without_password_(request);
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    httpd_req_t *req = *request;
    if (this->admitted_(request)) {
      // Let in, and still here: it is the upload with no password set.
      httpd_resp_set_status(req, "403 Forbidden");
      httpd_resp_set_type(req, "text/plain; charset=utf-8");
      httpd_resp_set_hdr(req, "Cache-Control", "no-store");
      httpd_resp_send(req, NO_UPLOAD, sizeof(NO_UPLOAD) - 1);
      return;
    }
    httpd_resp_set_status(req, "401 Unauthorized");  // ESP-IDF has no HTTPD_401 macro; ESPHome defines its own, privately
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ice9\", charset=\"UTF-8\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, REFUSED, sizeof(REFUSED) - 1);
  }

 protected:
  bool admitted_(AsyncWebServerRequest *request) const {
    auto auth = request->get_header("Authorization");
    if (!auth.has_value())
      return this->parent_->admits(nullptr, 0);
    return this->parent_->admits(auth->c_str(), auth->size());
  }
  bool upload_without_password_(AsyncWebServerRequest *request) const {
    if (request->method() != HTTP_POST || !is_update(request))
      return false;
    bool set, pending;
    this->parent_->state(set, pending);
    if (set)
      return false;  // and admitted_() has already seen the password
    if (this->parent_->upload_window_s() != 0)
      return false;  // just plugged in: hands on the box
#ifdef USE_CAPTIVE_PORTAL
    auto *portal = captive_portal::global_captive_portal;
    if (portal != nullptr && portal->is_active())
      return false;  // the setup hotspot: the card's password is the lock
#endif
    return true;
  }
  // A function of its own, never inlined: the URL buffer is half a kilobyte, off the web server
  // task's small stack again before anything else runs.
  static bool __attribute__((noinline)) is_update(AsyncWebServerRequest *request) {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(url_buf) == "/update";
  }
  PageLock *parent_;
};

#ifdef ICE9_SETUP_SCREEN
// While the setup hotspot is up, the phone gets the ice9 page, which draws its own setup screen
// (page/ice9.js, "the setup screen"), in place of ESPHome's. ESPHome's captive portal answers every
// GET with its own setup page while it is active, so this is asked before it (added straight after
// the gate) and takes three kinds of request:
//   /  /0.css  /0.js       handed to the web server, as on the van's Wi-Fi: the page and its two files
//   /config.json /wifisave not taken: the portal's own, and what the setup screen talks to
//   /ice9/...              not taken: the password and about handlers, and one way back (below)
//   anything else          sent to http://<the hotspot's address>/. That is what a phone asks for to
//                          find out whether a Wi-Fi wants a login (captive.apple.com and the rest):
//                          sent on, its pop-up opens on the ice9 page.
// The way back: http://192.168.4.1/ice9/plain-setup still gets ESPHome's own setup page (nothing
// here takes it, so the portal does), firmware upload and all. For support, if the setup screen
// ever fails someone in a van: it is not linked from anywhere.
// Once the box has joined a Wi-Fi the portal is off and this takes nothing.
class SetupScreen : public AsyncWebHandler {
 public:
  explicit SetupScreen(web_server::WebServer *web) : web_(web) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    auto *portal = captive_portal::global_captive_portal;
    if (portal == nullptr || !portal->is_active() || request->method() != HTTP_GET)
      return false;
    return what(request) != LEAVE;
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    if (what(request) == PAGE) {
      this->web_->handleRequest(request);
      return;
    }
    // The hotspot's own address, as lwIP writes it (192.168.4.1 unless a build says otherwise).
    char to[64] = "http://";
    ip_addr_t ip = wifi::global_wifi_component->wifi_soft_ap_ip();
    ipaddr_ntoa_r(&ip, to + 7, 46);
    strncat(to, "/", sizeof(to) - strlen(to) - 1);
    request->redirect(to);
  }

 protected:
  enum What : uint8_t { LEAVE, PAGE, ELSEWHERE };
  // A function of its own, never inlined: the URL buffer is half a kilobyte, off the web server
  // task's small stack again before anything else runs (the web server takes its own next).
  static What __attribute__((noinline)) what(AsyncWebServerRequest *request) {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const auto url = request->url_to(url_buf);
    if (url == "/" || url == "/0.css" || url == "/0.js")
      return PAGE;
    if (url == "/config.json" || url == "/wifisave" || (url.length() >= 6 && memcmp(url.c_str(), "/ice9/", 6) == 0))
      return LEAVE;
    return ELSEWHERE;
  }
  web_server::WebServer *web_;
};
#endif  // ICE9_SETUP_SCREEN

class PasswordHandler : public AsyncWebHandler {
 public:
  explicit PasswordHandler(PageLock *parent) : parent_(parent) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET && request->method() != HTTP_POST)
      return false;
    return is_ours(request);
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    // Only reached once the gate has let the request through: with a password set, whoever is
    // asking has already given it.
    if (request->method() == HTTP_POST) {
      if (!request->hasArg("new")) {
        request->send(400, "text/plain; charset=utf-8", "Send the new password as the form field new (empty to remove it).");
        return;
      }
      std::string why;
      int code = 400;
      if (!this->parent_->change(request->arg("new"), request->arg("current"), why, code)) {
        request->send(code, "text/plain; charset=utf-8", why.c_str());
        return;
      }
    }
    bool set, pending;
    this->parent_->state(set, pending);
    // "window": seconds left in which a firmware file is taken with no password set (0 = shut).
    char json[64];
    snprintf(json, sizeof(json), R"({"set":%s,"pending":%s,"window":%u})", set ? "true" : "false",
             pending ? "true" : "false", (unsigned) this->parent_->upload_window_s());
    request->send(200, "application/json", json);
  }

 protected:
  // A function of its own, never inlined: the URL buffer is half a kilobyte, off the web server
  // task's small stack again before anything else runs.
  static bool __attribute__((noinline)) is_ours(AsyncWebServerRequest *request) {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(url_buf) == "/ice9/password";
  }
  PageLock *parent_;
};

uint32_t PageLock::upload_window_s() const {
  if (!this->plugged_in_)
    return 0;
  // Time since this start, from a clock that doesn't wrap (millis() comes round after 49 days).
  const int64_t up_s = esp_timer_get_time() / 1000000;
  return up_s < (int64_t) UPLOAD_WINDOW_S ? (uint32_t) (UPLOAD_WINDOW_S - up_s) : 0;
}

// GET /ice9/about: what this box is, for the page's Settings and for support. The serial is the
// 6 characters on the end of its name (the last half of its MAC): the one printed on the case.
class AboutHandler : public AsyncWebHandler {
 public:
  explicit AboutHandler(PageLock *parent) : parent_(parent) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    return request->method() == HTTP_GET && is_ours(request);
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    const std::string mac = get_mac_address();  // 12 hex digits, lower case
#ifdef ESPHOME_PROJECT_VERSION
    static const char VERSION[] = ESPHOME_PROJECT_VERSION;
#else
    static const char VERSION[] = "";
#endif
    char json[192];
    snprintf(json, sizeof(json), R"({"name":"%s","serial":"%s","version":"%s","esphome":"%s","window":%u})",
             App.get_name().c_str(), mac.size() == 12 ? mac.c_str() + 6 : "", VERSION, ESPHOME_VERSION,
             (unsigned) this->parent_->upload_window_s());
    request->send(200, "application/json", json);
  }

 protected:
  static bool __attribute__((noinline)) is_ours(AsyncWebServerRequest *request) {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(url_buf) == "/ice9/about";
  }
  PageLock *parent_;
};

void PageLock::setup() {
  // Plugged in (or its reset button pressed), as against restarted by the page, an update or a
  // crash. Read once: it can't change while the box is up.
  this->plugged_in_ = esp_reset_reason() == ESP_RST_POWERON;
  this->pref_ = global_preferences->make_preference<Stored>(fnv1_hash("ice9_page_password"), true);
  Stored s{};
  {
    LockGuard guard(this->lock_);
    if (this->pref_.load(&s) && s.magic == MAGIC) {
      s.password[MAX_LEN] = '\0';
      const size_t len = strlen(s.password);
      if (len >= MIN_LEN && printable(std::string(s.password, len)))
        memcpy(this->cur_, s.password, sizeof(this->cur_));
    }
  }
  // Before any other handler (this runs first of all the components that add one): the web server
  // asks its handlers in the order they were added, and the first that takes a request has it.
  // Without ESPHome's own check: this is the check.
  web_server_base::global_web_server_base->add_handler_without_auth(new Gate(this));       // NOLINT
#ifdef ICE9_SETUP_SCREEN
  if (this->web_ != nullptr)
    web_server_base::global_web_server_base->add_handler_without_auth(new SetupScreen(this->web_));  // NOLINT
#endif
  web_server_base::global_web_server_base->add_handler(new PasswordHandler(this));         // NOLINT
  web_server_base::global_web_server_base->add_handler(new AboutHandler(this));            // NOLINT
}

bool PageLock::admits(const char *authorization, size_t len) {
  LockGuard guard(this->lock_);
  if (this->cur_[0] == '\0')
    return true;
  // "Basic <base64 of user:password>": the user name is whatever is before the first colon, and
  // ignored. The decoded form is at most 3/4 of the base64, so 64 characters of password and a
  // long user name fit in 192 bytes; anything longer can't be ours.
  static const size_t PREFIX = 6;
  if (authorization == nullptr || len <= PREFIX || strncmp(authorization, "Basic ", PREFIX) != 0 || len - PREFIX > 256)
    return false;
  uint8_t plain[193];
  const size_t n = base64_decode(reinterpret_cast<const uint8_t *>(authorization + PREFIX), len - PREFIX, plain,
                                 sizeof(plain) - 1);
  if (n == 0 || n >= sizeof(plain))
    return false;
  const char *colon = static_cast<const char *>(memchr(plain, ':', n));
  if (colon == nullptr)
    return false;
  const char *pw = colon + 1;
  const size_t pw_len = n - (size_t) (pw - reinterpret_cast<const char *>(plain));
  if (same(this->cur_, pw, pw_len)) {
    if (this->trial_) {
      // Someone has logged in with the new password: it's kept.
      this->trial_ = false;
      this->prev_[0] = '\0';
      this->save_ = true;
      this->say_ = Say::KEPT;
      this->busy_ = true;
    }
    return true;
  }
  // On trial, ONLY the new password gets in. The old one used to as well, and then a change made
  // from a phone that was logged in never stuck: its browser went on sending the old login, was
  // never asked for the new one, nobody logged in with it, and after 10 minutes the box went back
  // (found 25 Nov 2026). Now the old login gets a 401 and the browser asks. A new one
  // typed wrong still can't lock anyone out: 10 minutes, or the plug pulled, and the old one is back.
  return false;
}

void PageLock::state(bool &set, bool &pending) {
  LockGuard guard(this->lock_);
  set = this->cur_[0] != '\0';
  pending = this->trial_;
}

bool PageLock::change(const std::string &password, const std::string &current, std::string &why, int &code) {
  if (!password.empty()) {
    if (password.size() < MIN_LEN || password.size() > MAX_LEN) {
      why = "The password must be 4 to 64 characters.";
      return false;
    }
    if (!printable(password)) {
      why = "Letters, numbers, spaces and ordinary punctuation only: no accents or emoji.";
      return false;
    }
  }
  LockGuard guard(this->lock_);
  if (this->cur_[0] != '\0') {
    // A login the browser remembers isn't knowing the password: whoever picks up a phone that is
    // logged in can work the aircon, but changing or removing the password also needs it typed.
    const uint32_t now = millis();
    if (this->refused_ >= MAX_WRONG && now - this->refused_at_ms_ < WRONG_PAUSE_MS) {
      why = "Too many wrong tries: wait 10 minutes before trying again.";
      code = 403;  // not 429: this web server only sends a handful of codes, and anything else goes out as 500
      return false;
    }
    // On trial, the current password is the new one: nobody gets this far without it (the gate).
    const bool right = same(this->cur_, current.c_str(), current.size());
    if (!right) {
      if (this->refused_ >= MAX_WRONG)
        this->refused_ = 0;  // the pause is over: a fresh set of tries
      this->refused_++;
      this->refused_at_ms_ = now;
      why = "That isn't the current password.";
      code = 403;
      return false;
    }
    this->refused_ = 0;
  }
  if (password.empty()) {
    // Removing can't lock anyone out: at once, and kept.
    this->cur_[0] = '\0';
    this->prev_[0] = '\0';
    this->trial_ = false;
    this->save_ = true;
    this->say_ = Say::REMOVED;
  } else {
    // On trial: nothing goes to flash until someone logs in with it (admits()). What it goes back
    // to is what is in flash now, so a power cut before then goes back too.
    if (!this->trial_)
      memcpy(this->prev_, this->cur_, sizeof(this->prev_));
    strncpy(this->cur_, password.c_str(), MAX_LEN);
    this->cur_[MAX_LEN] = '\0';
    this->trial_ = true;
    this->trial_since_ms_ = millis();
    this->say_ = Say::ON_TRIAL;
  }
  this->busy_ = true;
  return true;
}

void PageLock::loop() {
  if (!this->busy_ && !this->trial_)
    return;  // nearly always: no lock taken
  Stored s{};
  bool save = false;
  Say say = Say::NOTHING;
  {
    LockGuard guard(this->lock_);
    if (this->trial_ && millis() - this->trial_since_ms_ > TRIAL_MS) {
      // Nobody logged in with the new one: back to how it was. Flash already holds that.
      const bool had = this->prev_[0] != '\0';
      memcpy(this->cur_, this->prev_, sizeof(this->cur_));
      this->prev_[0] = '\0';
      this->trial_ = false;
      this->say_ = had ? Say::WENT_BACK_OLD : Say::WENT_BACK_NONE;
    }
    save = this->save_;
    this->save_ = false;
    say = this->say_;
    this->say_ = Say::NOTHING;
    this->busy_ = false;
    if (save) {
      s.magic = MAGIC;
      memcpy(s.password, this->cur_, sizeof(s.password));
    }
  }
  if (save) {
    this->pref_.save(&s);
    global_preferences->sync();  // at once: a password that came back open after a power cut would be a surprise
  }
  switch (say) {
    case Say::ON_TRIAL:
      ESP_LOGI(TAG, "Page password set: it is kept once someone logs in with it, or goes back in 10 minutes");
      break;
    case Say::KEPT:
      ESP_LOGI(TAG, "Page password kept: someone logged in with it");
      break;
    case Say::REMOVED:
      ESP_LOGI(TAG, "Page password removed: the page is open to the van's Wi-Fi");
      break;
    case Say::WENT_BACK_NONE:
      ESP_LOGI(TAG, "Page password not used within 10 minutes: gone again, the page is open");
      break;
    case Say::WENT_BACK_OLD:
      ESP_LOGI(TAG, "New page password not used within 10 minutes: back to the old one");
      break;
    default:
      break;
  }
}

void PageLock::dump_config() {
  bool set, pending;
  this->state(set, pending);
  ESP_LOGCONFIG(TAG, "Page password: %s (any user name)", !set ? "off" : pending ? "on trial" : "set");
  ESP_LOGCONFIG(TAG, "Firmware upload with no password: %s",
                this->plugged_in_ ? "for 10 minutes from this plug-in" : "shut (this start was not a plug-in)");
}

}  // namespace ice9_lock
}  // namespace esphome
