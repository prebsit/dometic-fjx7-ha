// Tests for components/ice9_log/history_logic.h (F451's, copied with its tests): the records the log keeps in flash, and how
// they read back. No board needed:
//   c++ -std=c++17 -Wall -Wextra -o /tmp/history_test tests/history_logic_test.cpp && /tmp/history_test
#include "../components/ice9_log/history_logic.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace esphome::ice9_log::history;

static int failures = 0;
#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::cout << "FAILED line " << __LINE__ << ": " #cond "\n"; \
      failures++; \
    } \
  } while (0)

static std::string shown(const std::vector<std::string> &records) {
  std::string out;
  int day = -1;
  for (const auto &r : records)
    render_record(r.c_str(), day, out);
  return out;
}

int main() {
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);  // the van's
  tzset();

  // --- a record ---
  // 24 Nov 2026 02:45:10 GMT = 1795488310
  const uint32_t t = 1795488310UL;
  CHECK(make_record(true, t, "Flame: lit", 140) == "T6b04fa36Flame: lit");
  CHECK(make_record(false, 125, "Booted: power on", 140) == "U0000007dBooted: power on");
  CHECK(make_record(true, 0xFFFFFFFFUL, "", 140) == "Tffffffff");

  // --- reading it back ---
  CHECK(shown({make_record(true, t, "Flame: lit", 140)}) == "--- Tue 24 Nov ---\n02:45:10 Flame: lit\n");
  CHECK(shown({make_record(false, 125, "Booted: power on", 140)}) == "+2:05 Booted: power on\n");
  CHECK(shown({make_record(false, 0, "x", 140)}) == "+0:00 x\n");
  CHECK(shown({make_record(false, 3600 + 59, "x", 140)}) == "+60:59 x\n");
  // One heading a day, however many lines; a line before the clock was set gets none.
  CHECK(shown({make_record(true, t, "a", 140), make_record(true, t + 60, "b", 140), make_record(false, 5, "c", 140),
               make_record(true, t + 86400, "d", 140)}) ==
        "--- Tue 24 Nov ---\n02:45:10 a\n02:46:10 b\n+0:05 c\n--- Wed 25 Nov ---\n02:45:10 d\n");
  // Midnight is the change of day, in the van's own time: 23:59:59 and 00:00:00.
  CHECK(shown({make_record(true, 1795478399UL, "late", 140), make_record(true, 1795478400UL, "early", 140)}) ==
        "--- Mon 23 Nov ---\n23:59:59 late\n--- Tue 24 Nov ---\n00:00:00 early\n");
  // Summer time: 1 Jul 2026 12:00:00 UTC is 13:00 in the van. A single-figure day keeps its nought.
  CHECK(shown({make_record(true, 1782907200UL, "summer", 140)}) == "--- Wed 01 Jul ---\n13:00:00 summer\n");
  // The same day number in another year is another day.
  {
    std::string out;
    int day = -1;
    render_record(make_record(true, t, "a", 140).c_str(), day, out);
    const int first = day;
    render_record(make_record(true, t + 365UL * 86400UL, "b", 140).c_str(), day, out);
    CHECK(day != first);
  }
  // Not records: nothing is added, the day is left alone.
  {
    std::string out = "kept";
    int day = 7;
    CHECK(!render_record(nullptr, day, out));
    CHECK(!render_record("", day, out));
    CHECK(!render_record("T12345", day, out));           // too short
    CHECK(!render_record("X6b04f236text", day, out));    // not a kind we write
    CHECK(!render_record("T6b04f23Gtext", day, out));    // not a number
    CHECK(!render_record("T6B04F236text", day, out));    // we write small letters only
    CHECK(out == "kept" && day == 7);
  }

  // --- long lines ---
  {
    std::string s(200, 'x');
    cut(s, 140);
    CHECK(s.size() == 140 && s.substr(137) == "..." && s[136] == 'x');
    std::string fits(140, 'y');
    cut(fits, 140);
    CHECK(fits == std::string(140, 'y'));
    // Never half a character: "é" is two bytes, and the cut would land in the middle of one.
    std::string accents;
    for (int i = 0; i < 100; i++)
      accents += "\xC3\xA9";
    cut(accents, 140);
    CHECK(accents.size() <= 140 && accents.substr(accents.size() - 3) == "...");
    CHECK((accents.size() - 3) % 2 == 0);
    cut(accents, 2);  // nonsense limit: left alone
    CHECK(accents.size() > 2);
    CHECK(make_record(true, t, std::string(500, 'z'), 140).size() == STAMP_LEN + 140);
  }

  // --- the ring ---
  CHECK(slot_of(0, 1200) == 0 && slot_of(1199, 1200) == 1199 && slot_of(1200, 1200) == 0 && slot_of(2401, 1200) == 1);
  CHECK(first_kept(0, 1200) == 0 && first_kept(5, 1200) == 0 && first_kept(1200, 1200) == 0);
  CHECK(first_kept(1201, 1200) == 1 && first_kept(5000, 1200) == 3800);
  {
    char key[12];
    slot_key(key, 0);
    CHECK(strcmp(key, "l0") == 0);
    slot_key(key, 7);
    CHECK(strcmp(key, "l7") == 0);
    slot_key(key, 1199);
    CHECK(strcmp(key, "l1199") == 0);
    slot_key(key, 4294967295UL);
    CHECK(strcmp(key, "l4294967295") == 0);  // 11 characters and the end: why key needs 12
  }
  // A ring played through: write 3000 lines into 1200 slots, read back what first_kept says is there.
  {
    const uint32_t slots = 1200;
    std::vector<uint32_t> flash(slots, 0xFFFFFFFFUL);
    uint32_t next = 0;
    for (; next < 3000; next++)
      flash[slot_of(next, slots)] = next;
    uint32_t expect = first_kept(next, slots);
    CHECK(expect == 1800);
    bool in_order = true;
    for (uint32_t n = first_kept(next, slots); n != next; n++)
      in_order = in_order && flash[slot_of(n, slots)] == expect++;
    CHECK(in_order && expect == 3000);
  }

  // --- too many at once ---
  {
    Limiter l(120, 20000);
    uint32_t now = 5000000;  // not from zero: the first line can come at any time
    int kept = 0;
    for (int i = 0; i < 500; i++)
      kept += l.take(now) ? 1 : 0;
    CHECK(kept == 120);
    CHECK(!l.take(now + 19999));
    CHECK(l.take(now + 20000));
    CHECK(!l.take(now + 20001));
    // A quiet hour fills it again, and no further.
    now += 3600000;
    kept = 0;
    for (int i = 0; i < 500; i++)
      kept += l.take(now) ? 1 : 0;
    CHECK(kept == 120);
    // The millisecond clock wrapping round (every 49 days) is just more time going by.
    Limiter w(2, 1000);
    CHECK(w.take(0xFFFFFF00UL) && w.take(0xFFFFFF00UL) && !w.take(0xFFFFFF00UL));
    CHECK(w.take(0x00000300UL));  // 1024 ms later, across the wrap
    CHECK(!w.take(0x00000300UL));
    // A steady trickle slower than the refill is never refused.
    Limiter s(3, 1000);
    bool all = true;
    for (uint32_t i = 0; i < 1000; i++)
      all = all && s.take(i * 1000);
    CHECK(all);
  }

  if (failures) {
    std::cout << failures << " check(s) FAILED\n";
    return 1;
  }
  std::cout << "history logic: all checks passed\n";
  return 0;
}
