#!/usr/bin/env bash
# Tests the page password (components/ice9_lock) on a real board, over the network.
#
#   tools/lock-board-test.sh <address>          part 1: set, keep, change, and leave a change on trial
#   (pull the board's plug, put it back)
#   tools/lock-board-test.sh <address> after    part 2: the trial must be gone, the kept one back; then tidy up
#   tools/lock-board-test.sh <address> clear    take off a password this test left behind (after a FAIL)
#
# <address> is the board's number or name, e.g. 192.168.1.50 or ice9-abcdef.local
#
# For a TEST board only. It refuses to start on a box that already has a page password, and the
# passwords it uses are the three below, so it can never take off a password it didn't set.
#
# What part 1 is really for (the bug of 25 Nov 2026): after a change, the OLD password must get
# a 401 while the new one is on trial. Before the fix it got a 200, so a logged-in phone was never
# asked for the new one and the change never stuck.
set -u
A="${1:-}"
MODE="${2:-first}"
if [ -z "$A" ]; then
  echo "Which board? e.g.  tools/lock-board-test.sh 192.168.1.50"
  exit 2
fi
U="http://$A/ice9/password"
ABOUT="http://$A/ice9/about"
P1=first123
P2=second12
P3=third123
fails=0

code() { curl -s -o /dev/null -w '%{http_code}' -m 10 "$@"; }
body() { curl -s -m 10 "$@"; }
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
# has <what it proves> <what came back> <what must be in it>
has() {
  case "$2" in
    *"$3"*) pass "$1" ;;
    *) fail "$1   (wanted $3, got: ${2:-nothing})" ;;
  esac
}
is() {
  if [ "$2" = "$3" ]; then pass "$1"; else fail "$1   (wanted $3, got $2)"; fi
}
reach() {
  # Up to 90 s: a board that has just been plugged in needs a while to join the Wi-Fi.
  local n=0 c
  while [ $n -lt 30 ]; do
    c=$(code "$ABOUT")
    if [ "$c" != "000" ]; then return 0; fi
    n=$((n + 1))
    sleep 3
  done
  echo "Can't reach $A (no answer in 90 s). Is it on the Wi-Fi, and is this the right address?"
  exit 2
}
finish() {
  echo
  if [ $fails -eq 0 ]; then
    echo "ALL PASS ($1)"
  else
    echo "$fails FAILED ($1). If the board is left with a password:  $0 $A clear"
  fi
  exit $fails
}

reach
# With a password set, about needs it too. After part 1 the kept one is P2.
if [ "$MODE" = "after" ]; then
  about=$(body -u "x:$P2" "$ABOUT")
else
  about=$(body "$ABOUT")
fi
case "$about" in
  *'"serial"'*) echo "Board: $about" ;;
  *) echo "Board: (it wants a password before it says)" ;;
esac
echo

if [ "$MODE" = "clear" ]; then
  for p in "$P1" "$P2" "$P3"; do
    body -u "x:$p" -d "new=" -d "current=$p" "$U" >/dev/null
  done
  is "no password left on the board" "$(code "$U")" 200
  finish "clear"
fi

if [ "$MODE" = "after" ]; then
  # Nothing here sends the on-trial password until the plug is known to have been pulled: a login
  # with it would keep it, and the test would have changed what it was testing.
  case "$about" in
    *'"window":0}'*)
      echo "STOPPED: this start was not a plug-in (the upload window is shut), so part 2 would prove nothing."
      echo "Run part 1 again, then pull the plug:  $0 $A clear ; $0 $A"
      exit 2 ;;
    *'"window":'*) pass "this start was a plug-in (the upload window is open)" ;;
    *)
      echo "STOPPED: the kept password was refused. Either the plug has not been pulled yet (the change is"
      echo "still on trial: pull it and run this again), or a power cut did not bring the kept password back."
      exit 2 ;;
  esac
  is "no login is still refused: the kept password survived the power cut" "$(code "$U")" 401
  is "the change that was on trial is gone: its password is refused" "$(code -u "x:$P3" "$U")" 401
  has "the kept password is in, and not on trial" "$(body -u "x:$P2" "$U")" '"set":true,"pending":false'
  has "tidy up: password removed" "$(body -u "x:$P2" -d "new=" -d "current=$P2" "$U")" '"set":false'
  is "the page is open again with no login" "$(code "$U")" 200
  finish "part 2"
fi

# Part 1. Only on a box with no page password.
c=$(code "$U")
if [ "$c" = "401" ]; then
  echo "This box already has a page password. Not touching it."
  echo "(If an earlier run of this test left it:  $0 $A clear)"
  exit 2
fi
has "starts with no password" "$(body "$U")" '"set":false'

has "set one: it is on trial" "$(body -d "new=$P1" "$U")" '"set":true,"pending":true'
is "no login is refused" "$(code "$U")" 401
has "log in with it: kept" "$(body -u "x:$P1" "$U")" '"set":true,"pending":false'

is "a wrong current password can't change it" "$(code -u "x:$P1" -d "new=$P2" -d "current=nope1234" "$U")" 403
has "change it: the new one is on trial" "$(body -u "x:$P1" -d "new=$P2" -d "current=$P1" "$U")" '"set":true,"pending":true'
is "THE FIX: the old password is refused while the new one is on trial" "$(code -u "x:$P1" "$U")" 401
is "no login is still refused" "$(code "$U")" 401
has "log in with the new one: kept" "$(body -u "x:$P2" "$U")" '"set":true,"pending":false'
is "the old password stays refused" "$(code -u "x:$P1" "$U")" 401

has "change it again and leave it on trial" "$(body -u "x:$P2" -d "new=$P3" -d "current=$P2" "$U")" '"set":true,"pending":true'

if [ $fails -eq 0 ]; then
  echo
  echo "Part 1: ALL PASS. A change is now on trial and nobody has logged in with it."
  echo "Pull the board's plug, count to five, put it back and LEAVE it in. Then:"
  echo "  $0 $A after"
  exit 0
fi
finish "part 1"
