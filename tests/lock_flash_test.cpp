// The page password's gate and its trial, as components/ice9_lock/page_lock.cpp really has them,
// run on this computer. tools/lock-host-test.sh copies the functions out of page_lock.cpp word for
// word (into lock_extracted.inc) and builds this around them, with a pretend clock, flash and log.
//
//   tools/lock-host-test.sh
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// ---- pretend board ----
static uint32_t fake_ms = 1000;
static uint32_t millis() { return fake_ms; }
static bool lock_held = false;
class Mutex { public: void lock() { assert(!lock_held && "locked twice: a deadlock on the board"); lock_held = true; } void unlock() { lock_held = false; } };
class LockGuard { public: explicit LockGuard(Mutex &m) : m_(m) { m_.lock(); } ~LockGuard() { m_.unlock(); } Mutex &m_; };
static std::vector<std::string> logged;
#define ESP_LOGI(tag, fmt, ...) logged.push_back(fmt)
static const char *const TAG = "ice9.lock";
static std::string flash;  // what the board would find after a power cut
static int flash_writes = 0;
struct Prefs { void sync() {} };
static Prefs prefs, *global_preferences = &prefs;

// esphome/core/helpers.cpp's base64_decode, the same behaviour: stops at '=' or anything else.
static size_t base64_decode(const uint8_t *in, size_t len, uint8_t *buf, size_t buf_len) {
  auto val = [](uint8_t c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  size_t out = 0;
  uint32_t acc = 0, bits = 0;
  for (size_t i = 0; i < len; i++) {
    int v = val(in[i]);
    if (v < 0) break;
    acc = (acc << 6) | (uint32_t) v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (out == buf_len) return out;
      buf[out++] = (uint8_t) (acc >> bits);
    }
  }
  return out;
}
static std::string b64(const std::string &s) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  size_t i = 0;
  for (; i + 2 < s.size(); i += 3) {
    uint32_t n = ((uint8_t) s[i] << 16) | ((uint8_t) s[i + 1] << 8) | (uint8_t) s[i + 2];
    o += T[n >> 18]; o += T[(n >> 12) & 63]; o += T[(n >> 6) & 63]; o += T[n & 63];
  }
  if (i + 1 == s.size()) { uint32_t n = (uint8_t) s[i] << 16; o += T[n >> 18]; o += T[(n >> 12) & 63]; o += "=="; }
  else if (i + 2 == s.size()) { uint32_t n = ((uint8_t) s[i] << 16) | ((uint8_t) s[i + 1] << 8); o += T[n >> 18]; o += T[(n >> 12) & 63]; o += T[(n >> 6) & 63]; o += '='; }
  return o;
}

namespace esphome {
namespace ice9_lock {
class PageLock {
 public:
  bool admits(const char *authorization, size_t len);
  void state(bool &set, bool &pending);
  bool change(const std::string &password, const std::string &current, std::string &why, int &code);
  void loop();
  static const size_t MAX_LEN = 64;
  static const uint8_t MAX_WRONG = 5;
  static const uint32_t WRONG_PAUSE_MS = 10UL * 60UL * 1000UL;
  static const uint32_t TRIAL_MS = 10UL * 60UL * 1000UL;
  struct Stored { uint32_t magic; char password[MAX_LEN + 1]; };
  enum class Say : uint8_t { NOTHING, ON_TRIAL, KEPT, REMOVED, WENT_BACK_NONE, WENT_BACK_OLD };
  struct Pref { void save(const Stored *s) { flash = s->password; flash_writes++; } } pref_;
  Mutex lock_;
  char cur_[MAX_LEN + 1]{};
  char prev_[MAX_LEN + 1]{};
  bool trial_{false};
  uint32_t trial_since_ms_{0};
  bool save_{false};
  Say say_{Say::NOTHING};
  uint8_t refused_{0};
  uint32_t refused_at_ms_{0};
  volatile bool busy_{false};
};
#include "lock_extracted.inc"
}  // namespace ice9_lock
}  // namespace esphome
using esphome::ice9_lock::PageLock;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::cout << "FAILED line " << __LINE__ << ": " #cond "\n"; failures++; } } while (0)

static bool in(PageLock &p, const std::string &user, const std::string &pw) {
  std::string h = "Basic " + b64(user + ":" + pw);
  return p.admits(h.c_str(), h.size());
}
static bool in_raw(PageLock &p, const char *h) { return p.admits(h, h == nullptr ? 0 : strlen(h)); }
static bool set_pw(PageLock &p, const std::string &now, const std::string &current, int *code = nullptr) {
  std::string why; int c = 400;
  bool ok = p.change(now, current, why, c);
  if (code) *code = ok ? 200 : c;
  return ok;
}
static bool said(const char *start) { for (auto &l : logged) if (l.rfind(start, 0) == 0) return true; return false; }
static void tick(PageLock &p, uint32_t ms) { fake_ms += ms; p.loop(); }

int main() {
  // --- no password: everything gets in ---
  {
    PageLock p;
    CHECK(in_raw(p, nullptr) && in_raw(p, "Basic garbage") && in(p, "", ""));
  }
  // --- set: on trial, nothing in flash, any user name gets in with it ---
  {
    PageLock p; flash = ""; logged.clear(); flash_writes = 0;
    CHECK(set_pw(p, "test1234", ""));
    tick(p, 10);
    CHECK(said("Page password set") && flash_writes == 0);
    bool set, pending; p.state(set, pending); CHECK(set && pending);
    CHECK(!in_raw(p, nullptr));                 // no login: refused (and the browser asks)
    CHECK(!in(p, "ice9", "wrong"));
    CHECK(!in(p, "anything", "test123"));       // one short
    CHECK(!in(p, "anything", "test12345"));     // one long
    CHECK(in(p, "someone", "test1234"));          // any user name: kept
    tick(p, 10);
    CHECK(said("Page password kept") && flash == "test1234" && flash_writes == 1);
    p.state(set, pending); CHECK(set && !pending);
    CHECK(in(p, "", "test1234") && in(p, "ice9", "test1234") && in(p, "a:b", "x") == false);
    // A colon in the password itself: everything after the FIRST colon is the password.
    // A user name with a colon in it isn't possible in Basic auth, so this is the only reading.
    CHECK(set_pw(p, "pa:ss:word", "test1234"));
    CHECK(in(p, "", "pa:ss:word"));
    CHECK(in(p, "x", "pa:ss:word"));
  }
  // --- set, then nobody logs in: back to open after 10 minutes, and flash never changed ---
  {
    PageLock p; flash = ""; logged.clear(); flash_writes = 0;
    CHECK(set_pw(p, "typo1234", ""));
    tick(p, 9UL * 60UL * 1000UL);
    CHECK(!in_raw(p, nullptr));                 // still on trial at 9 minutes
    tick(p, 61UL * 1000UL);
    CHECK(said("Page password not used within 10 minutes: gone again") && in_raw(p, nullptr));
    CHECK(flash_writes == 0);
  }
  // --- change while one is kept: on trial only the new one gets in, and it keeps it ---
  {
    PageLock p; flash = ""; logged.clear(); flash_writes = 0;
    set_pw(p, "old12345", ""); in(p, "", "old12345"); tick(p, 10);
    CHECK(flash == "old12345");
    int code;
    CHECK(!set_pw(p, "new12345", "", &code) && code == 403);        // current not typed
    CHECK(!set_pw(p, "new12345", "nope", &code) && code == 403);    // current wrong
    logged.clear();
    CHECK(set_pw(p, "new12345", "old12345"));
    CHECK(!in(p, "", "old12345"));              // the old one no longer gets in: the browser must ask
    CHECK(!in_raw(p, nullptr));
    tick(p, 10);
    CHECK(flash == "old12345" && !said("Page password kept"));
    bool set, pending; p.state(set, pending); CHECK(set && pending);
    tick(p, 9UL * 60UL * 1000UL);
    CHECK(!in(p, "", "old12345"));              // still refused at 9 minutes
    tick(p, 2UL * 60UL * 1000UL);               // nobody used the new one
    CHECK(said("New page password not used within 10 minutes: back to the old one"));
    CHECK(in(p, "", "old12345") && !in(p, "", "new12345") && flash == "old12345");
    // And this time it is used.
    CHECK(set_pw(p, "new12345", "old12345"));
    CHECK(in(p, "", "new12345")); tick(p, 10);
    CHECK(flash == "new12345" && !in(p, "", "old12345"));
  }
  // --- two changes during one trial: it still goes back to what was kept, not the first new one ---
  {
    PageLock p; flash = ""; logged.clear();
    set_pw(p, "kept1234", ""); in(p, "", "kept1234"); tick(p, 10);
    CHECK(set_pw(p, "first123", "kept1234"));
    int code;
    CHECK(!set_pw(p, "second12", "kept1234", &code) && code == 403);  // on trial the new one is current
    CHECK(set_pw(p, "second12", "first123"));
    CHECK(!in(p, "", "first123"));
    tick(p, 11UL * 60UL * 1000UL);
    CHECK(in(p, "", "kept1234") && !in(p, "", "second12"));
  }
  // --- remove: at once, kept, from on trial or not ---
  {
    PageLock p; flash = "x"; logged.clear();
    set_pw(p, "test1234", "");
    CHECK(set_pw(p, "", "test1234"));
    tick(p, 10);
    CHECK(said("Page password removed") && flash == "" && in_raw(p, nullptr));
    tick(p, 11UL * 60UL * 1000UL);              // no trial left to time out
    CHECK(in_raw(p, nullptr));
  }
  // --- refusals of a new password ---
  {
    PageLock p; int code;
    CHECK(!set_pw(p, "abc", "", &code) && code == 400);
    CHECK(!set_pw(p, std::string(65, 'a'), "", &code) && code == 400);
    CHECK(!set_pw(p, "caf\xC3\xA9" "1234", "", &code) && code == 400);
    CHECK(set_pw(p, std::string(64, 'a'), ""));
    CHECK(in(p, "", std::string(64, 'a')));
  }
  // --- five wrong current passwords: changes paused for 10 minutes, the login still works ---
  {
    PageLock p; set_pw(p, "real1234", ""); in(p, "", "real1234"); tick(p, 10);
    int code;
    for (int i = 0; i < 5; i++) CHECK(!set_pw(p, "x1234567", "bad", &code) && code == 403);
    CHECK(!set_pw(p, "x1234567", "real1234", &code) && code == 403);   // even the right one, during the pause
    CHECK(in(p, "", "real1234"));
    fake_ms += 10UL * 60UL * 1000UL + 1;
    CHECK(set_pw(p, "x1234567", "real1234"));
  }
  // --- headers that aren't a login ---
  {
    PageLock p; set_pw(p, "test1234", ""); in(p, "", "test1234");
    CHECK(!in_raw(p, ""));
    CHECK(!in_raw(p, "Basic"));
    CHECK(!in_raw(p, "Basic "));
    CHECK(!in_raw(p, "Bearer dGVzdDE6dGVzdDEyMzQ="));
    CHECK(!in_raw(p, "Basic dGVzdDEyMzQ="));     // "test1234" with no colon
    std::string huge = "Basic " + std::string(400, 'A');
    CHECK(!in_raw(p, huge.c_str()));
    CHECK(in_raw(p, ("Basic " + b64(std::string(120, 'u') + ":test1234")).c_str()));   // a long user name still fits
  }
  if (failures) { std::cout << failures << " check(s) FAILED\n"; return 1; }
  std::cout << "page password: all checks passed\n";
  return 0;
}
