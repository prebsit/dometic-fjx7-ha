#pragma once
// The history's own arithmetic and wording, with no board in it, so it can be tested on a
// computer: tests/history_logic_test.cpp. F451's (esphome/components/wbus/history_logic.h in the
// f451 repository), word for word apart from the namespace: keep the two the same.
//
// Every line of the log is also kept in flash (kept_log.cpp), in a ring of numbered slots, as a
// "record":
//   'T' + 8 hex digits   the clock's time, seconds since 1970
//   'U' + 8 hex digits   seconds since F451 started (its clock wasn't set yet)
// and then the line's text. The time is put into words when the history is read, not when the
// line is written: a change of time zone or of summer time shows the right hour, and the day
// headings ("--- Tue 24 Nov ---") take no flash.

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

namespace esphome {
namespace ice9_log {
namespace history {

static const size_t STAMP_LEN = 9;  // the letter and its 8 digits

// Cut text to at most `max` bytes, ending in "...", without splitting a UTF-8 character.
inline void cut(std::string &s, size_t max) {
  if (s.size() <= max || max < 3)
    return;
  size_t at = max - 3;
  while (at > 0 && (static_cast<uint8_t>(s[at]) & 0xC0) == 0x80)
    at--;
  s.resize(at);
  s += "...";
}

inline std::string make_record(bool clock, uint32_t stamp, std::string text, size_t text_max) {
  static const char DIGITS[] = "0123456789abcdef";
  cut(text, text_max);
  std::string r;
  r.reserve(STAMP_LEN + text.size());
  r += clock ? 'T' : 'U';
  for (int shift = 28; shift >= 0; shift -= 4)
    r += DIGITS[(stamp >> shift) & 0xF];
  r += text;
  return r;
}

// The slot a line goes in, and the first line still kept once `next` lines have been written.
inline uint32_t slot_of(uint32_t line, uint32_t slots) { return line % slots; }
inline uint32_t first_kept(uint32_t next, uint32_t slots) { return next > slots ? next - slots : 0; }

// A slot's name in flash: "l0" ... "l1199". Written out by hand: this runs in the web server's
// task, whose stack is too small to spend on printf. `key` needs 12 bytes.
inline void slot_key(char *key, uint32_t slot) {
  char digits[10];
  int n = 0;
  do {
    digits[n++] = (char) ('0' + slot % 10);
    slot /= 10;
  } while (slot != 0);
  *key++ = 'l';
  while (n > 0)
    *key++ = digits[--n];
  *key = '\0';
}

inline void put2(std::string &out, unsigned v) {
  out += (char) ('0' + (v / 10) % 10);
  out += (char) ('0' + v % 10);
}

inline void put_number(std::string &out, uint32_t v) {
  char digits[10];
  int n = 0;
  do {
    digits[n++] = (char) ('0' + v % 10);
    v /= 10;
  } while (v != 0);
  while (n > 0)
    out += digits[--n];
}

// One number per day, for telling when the day has changed.
inline int day_key(const struct tm &lt) { return (lt.tm_year + 1900) * 400 + lt.tm_yday; }

// "--- Tue 24 Nov ---"
inline std::string day_marker(const struct tm &lt) {
  static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  std::string s = "--- ";
  s += DAYS[((lt.tm_wday % 7) + 7) % 7];
  s += ' ';
  put2(s, (unsigned) lt.tm_mday);
  s += ' ';
  s += MONTHS[((lt.tm_mon % 12) + 12) % 12];
  s += " ---";
  return s;
}

// Add a record to `out` as the log shows it, "HH:MM:SS text" (or "+M:SS text"), with a day
// heading first when it falls on another day than `day` (which is updated; start it at -1).
// False, and nothing added, if it isn't a record.
inline bool render_record(const char *rec, int &day, std::string &out) {
  if (rec == nullptr || (rec[0] != 'T' && rec[0] != 'U'))
    return false;
  uint32_t v = 0;
  for (size_t i = 1; i < STAMP_LEN; i++) {
    const char c = rec[i];
    uint32_t d;
    if (c >= '0' && c <= '9') {
      d = (uint32_t) (c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = (uint32_t) (c - 'a') + 10;
    } else {
      return false;  // the end of the text included
    }
    v = (v << 4) | d;
  }
  if (rec[0] == 'T') {
    const time_t t = (time_t) v;
    struct tm lt {};
    ::localtime_r(&t, &lt);
    const int key = day_key(lt);
    if (key != day) {
      day = key;
      out += day_marker(lt);
      out += '\n';
    }
    put2(out, (unsigned) lt.tm_hour);
    out += ':';
    put2(out, (unsigned) lt.tm_min);
    out += ':';
    put2(out, (unsigned) lt.tm_sec);
  } else {
    out += '+';
    put_number(out, v / 60);
    out += ':';
    put2(out, v % 60);
  }
  out += ' ';
  out += rec + STAMP_LEN;
  out += '\n';
  return true;
}

// How many lines may go to flash: `burst` at once, then one more every `every_ms`. A link that
// flaps all night logs thousands of lines; they mustn't push the rest of the week out.
class Limiter {
 public:
  Limiter(uint16_t burst, uint32_t every_ms) : burst_(burst), every_ms_(every_ms), tokens_(burst) {}
  bool take(uint32_t now_ms) {
    if (!this->started_) {
      this->started_ = true;
      this->last_ms_ = now_ms;
    }
    const uint32_t add = (now_ms - this->last_ms_) / this->every_ms_;
    if (add > 0) {
      this->tokens_ = add >= (uint32_t) (this->burst_ - this->tokens_) ? this->burst_ : (uint16_t) (this->tokens_ + add);
      this->last_ms_ += add * this->every_ms_;
    }
    if (this->tokens_ == 0)
      return false;
    this->tokens_--;
    return true;
  }

 protected:
  uint16_t burst_;
  uint32_t every_ms_;
  uint16_t tokens_;
  uint32_t last_ms_{0};
  bool started_{false};
};

}  // namespace history
}  // namespace ice9_log
}  // namespace esphome
