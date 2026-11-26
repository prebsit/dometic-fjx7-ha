#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include <string>

namespace esphome {
namespace web_server {
class WebServer;
}  // namespace web_server
namespace ice9_lock {

// The page password (see __init__.py). Off until the owner sets one.
class PageLock : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // First of everything, so the gate is the first handler the web server asks about every request.
  float get_setup_priority() const override { return setup_priority::BUS; }

  // From the web server's task, for every request: may it through? (Any user name; the password
  // is what counts.) A request with a new password that is still on trial keeps it; while one
  // is on trial, the old one doesn't get in.
  bool admits(const char *authorization, size_t len);
  void state(bool &set, bool &pending);
  // From the web server's task. Empty removes the password. When one is set, `current` must be
  // it. False, with the reason and an HTTP status, if refused.
  bool change(const std::string &password, const std::string &current, std::string &why, int &code);
  // Seconds left of the plug-in window (0 = shut): for this long after the box is plugged in, a
  // firmware file is taken with no page password set. A restart from the page or after an update
  // doesn't open it: it takes hands on the box, like the factory reset.
  uint32_t upload_window_s() const;

  // The web server that serves the page, given by __init__.py: the setup screen passes it the
  // page's own requests while the setup hotspot is up (page_lock.cpp, SetupScreen).
  void set_web_server(web_server::WebServer *web) { this->web_ = web; }

  static const size_t MAX_LEN = 64;

 protected:
  static const uint8_t MAX_WRONG = 5;  // wrong current passwords in a row, then a pause
  static const uint32_t WRONG_PAUSE_MS = 10UL * 60UL * 1000UL;
  // A new password is on trial until someone logs in with it, and until then it is the only one
  // that gets in. If nobody does in this long, the box goes back to how it was (losing power does
  // the same at once: nothing is in flash yet): setting a password can't lock anyone out for good.
  static const uint32_t TRIAL_MS = 10UL * 60UL * 1000UL;
  static const uint32_t UPLOAD_WINDOW_S = 10UL * 60UL;
  struct Stored {
    uint32_t magic;
    char password[MAX_LEN + 1];
  };
  enum class Say : uint8_t { NOTHING, ON_TRIAL, KEPT, REMOVED, WENT_BACK_NONE, WENT_BACK_OLD };

  ESPPreferenceObject pref_;
  Mutex lock_;
  // All of these are only touched with lock_ held: the web server's task checks every request
  // against them while the main loop may be saving or going back.
  char cur_[MAX_LEN + 1]{};   // what gets in now ("" = no password)
  char prev_[MAX_LEN + 1]{};  // on trial: what it goes back to (it doesn't get in meanwhile)
  bool trial_{false};
  uint32_t trial_since_ms_{0};
  bool save_{false};          // write cur_ to flash (main loop)
  Say say_{Say::NOTHING};     // a line for the log (main loop)
  uint8_t refused_{0};
  uint32_t refused_at_ms_{0};
  volatile bool busy_{false};  // something above for the main loop: looked at without the lock
  bool plugged_in_{false};     // this start was a power-on (set once in setup, read-only after)
  web_server::WebServer *web_{nullptr};
};

}  // namespace ice9_lock
}  // namespace esphome
