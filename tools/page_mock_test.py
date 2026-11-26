"""Drive the ice9 page against page/preview-mock.js in Chromium and WebKit (Playwright): the page password, the update, the setup screen, the power limit, the log, and X-Ice9-From on every request.
Run: uv run --no-project --with playwright python -I tools/page_mock_test.py [screenshot dir]   (about a minute)
WebKit is Safari's engine, not Safari on an iPhone; the login box is played by window.prompt()."""
import functools, http.server, sys, threading, time
from playwright.sync_api import sync_playwright, expect

PAGE = str(__import__("pathlib").Path(__file__).resolve().parent.parent / "page")
SHOTS = sys.argv[1] if len(sys.argv) > 1 else "/tmp"   # where the screenshots go

srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(http.server.SimpleHTTPRequestHandler, directory=PAGE))
srv.RequestHandlerClass.log_message = lambda *a: None
threading.Thread(target=srv.serve_forever, daemon=True).start()
BASE = f"http://127.0.0.1:{srv.server_address[1]}/preview.html"

results = []
def check(name, fn):
    try:
        fn(); results.append(("PASS", name))
    except Exception as e:
        results.append(("FAIL", f"{name}: {" | ".join(l for l in str(e).splitlines() if "Actual" in l or "Error" in l or "line" in l)[:600]}"))

FIRMWARE = {"name": "ice9-1.0.1.ota.bin", "mimeType": "application/octet-stream", "buffer": b"\xe9" + b"\0" * 200_000}
# A factory image as ESPHome makes it for the S3: bootloader at 0, partition table (AA 50) at 0x8000, app (E9) at 0x10000.
_factory = bytearray(b"\xff" * 0x30000); _factory[0] = 0xE9; _factory[0x8000:0x8002] = b"\xaa\x50"; _factory[0x10000] = 0xE9
FACTORY = {"name": "ice9-1.0.1.ota.bin", "mimeType": "application/octet-stream", "buffer": bytes(_factory)}   # renamed: the contents decide
NOT_FIRMWARE = {"name": "holiday.jpg", "mimeType": "image/jpeg", "buffer": b"\xff\xd8\xff\xe0" + b"\0" * 5000}

def run(bw, name):
    def fresh(query="", answers=(), screen="#config"):
        ctx = bw.new_context(viewport={"width": 390, "height": 844}, device_scale_factor=2)
        pg = ctx.new_page()
        pg.errors = []
        pg.on("pageerror", lambda e: pg.errors.append(str(e)))
        queue = list(answers)
        pg.prompts = []
        def on_dialog(d):
            pg.prompts.append(d.message)
            (d.accept(queue.pop(0)) if queue else d.dismiss())
        pg.on("dialog", on_dialog)
        pg.answers = queue
        pg.goto(BASE + query + screen)
        return ctx, pg
    card = lambda pg, title: pg.locator("section.card", has=pg.locator("h2", has_text=title))
    # Every request this page load made carried X-Ice9-From: page (fetch, the upload, the setup portal's two).
    def all_from_page(pg, at_least=1, must=()):
        asked = pg.evaluate("() => window.ice9Mock.asked")
        assert len(asked) >= at_least, asked
        wrong = [a for a in asked if a[1] != "page"]
        assert not wrong, f"without X-Ice9-From: page: {wrong}"
        for path in must: assert any(a[0] == path for a in asked), f"no request to {path}: {asked}"

    # 1. Set, change, remove, from a phone with no password to start with.
    def password_cycle():
        ctx, pg = fresh()
        pw = card(pg, "Page password")
        expect(pw.locator("p.state")).to_contain_text("Off. Anyone on the van's WiFi")
        expect(pw.locator("text=Forgotten it?")).to_be_visible()
        pg.screenshot(path=f"{SHOTS}/{name}-settings-off.png", full_page=True)
        pw.get_by_role("button", name="Set a password").click()
        pg.fill("#pw-new", "abc"); pg.fill("#pw-again", "abc"); pw.get_by_role("button", name="Set it").click()
        expect(pw.locator("p.said")).to_have_text("The password must be 4 to 64 characters.")
        pg.fill("#pw-new", "abcd1234"); pg.fill("#pw-again", "abcd1235"); pw.get_by_role("button", name="Set it").click()
        expect(pw.locator("p.said")).to_have_text("The two new passwords aren't the same.")
        pw.get_by_role("switch", name="Show what I type").click()
        assert pg.get_attribute("#pw-new", "type") == "text"
        pg.fill("#pw-new", "abcd1234"); pg.fill("#pw-again", "abcd1234")
        pg.answers.append("abcd1234")                      # what the person types into the login box
        with pg.expect_navigation():
            pw.get_by_role("button", name="Set it").click()
        expect(card(pg, "Page password").locator("p.state")).to_contain_text("On. The ice9 asks for it", timeout=5000)
        assert pg.prompts, "no login box after setting it"
        assert "abcd1234" not in pg.content(), "password left in the page"
        # Change: wrong current first, then right. On trial only the new one gets in, so the box asks again.
        pw = card(pg, "Page password")
        pw.get_by_role("button", name="Change").click()
        pg.fill("#pw-current", "nope"); pg.fill("#pw-new", "newpass99"); pg.fill("#pw-again", "newpass99")
        pw.get_by_role("button", name="Change it").click()
        expect(pw.locator("p.said")).to_have_text("That isn't the current password.")
        pg.fill("#pw-current", "abcd1234"); pg.fill("#pw-new", "newpass99"); pg.fill("#pw-again", "newpass99")
        n = len(pg.prompts); pg.answers.append("newpass99")
        with pg.expect_navigation():
            pw.get_by_role("button", name="Change it").click()
        expect(card(pg, "Page password").locator("p.state")).to_contain_text("On. The ice9 asks for it", timeout=5000)
        assert len(pg.prompts) == n + 1, f"expected one login box after the change, got {len(pg.prompts) - n}"
        pg.screenshot(path=f"{SHOTS}/{name}-settings-on.png", full_page=True)
        # Remove.
        pw = card(pg, "Page password")
        pw.get_by_role("button", name="Remove").click()
        pg.fill("#pw-current", "newpass99"); pw.get_by_role("button", name="Remove the password").click()
        expect(pw.locator("p.said")).to_have_text("Removed. Anyone on the van's WiFi can use this page again.")
        expect(pw.locator("p.state")).to_contain_text("Off.")
        all_from_page(pg, must=("/ice9/password", "/ice9/about", "/ice9/log"))
        assert not pg.errors, pg.errors
        ctx.close()
    check(f"{name}: set (checks, login box), change (wrong then right, one login box), remove", password_cycle)

    # 3. Update, no password, just plugged in: countdown, progress, restart, version before and after.
    def update_window():
        ctx, pg = fresh("?window=600")
        up = card(pg, "Update")
        expect(up.locator("p.state")).to_contain_text(r"for another 9:5", timeout=4000)
        this = card(pg, "This ice9")
        expect(this.locator(".kv", has_text="Serial").locator("b")).to_have_text("abcdef")
        expect(this.locator(".kv", has_text="Firmware").locator("b")).to_have_text("1.0.0-dev")
        pg.set_input_files("#upd-file", files=[FIRMWARE])
        up.get_by_role("button", name="Send").click()
        expect(up.locator("p.said")).to_contain_text("Sending:", timeout=3000)
        expect(up.locator("p.said")).to_contain_text("Keep this page open")
        expect(up.locator("p.said")).to_have_text("Sent. The ice9 is restarting: about 20 seconds.", timeout=8000)
        assert pg.locator(".banner").first.is_hidden(), "can't-reach banner shown during the update"
        expect(up.locator("p.said")).to_have_text("Back. Firmware 1.0.0-dev before, 1.0.1-mock now.", timeout=30000)
        # A restart after an update isn't a plug-in: the window is shut now.
        expect(up.locator("p.state")).to_contain_text("unplug the ice9, plug it back in")
        expect(up.locator("#upd-file")).to_be_hidden()
        expect(this.locator(".kv", has_text="Firmware").locator("b")).to_have_text("1.0.1-mock")
        all_from_page(pg, must=("/update",))
        pg.screenshot(path=f"{SHOTS}/{name}-update-done.png", full_page=True)
        assert not pg.errors, pg.errors
        ctx.close()
    check(f"{name}: update in the plug-in window (progress, restart, before/after, window shut after)", update_window)

    # 4. No password, window shut: no Send, says what to do.
    def update_shut():
        ctx, pg = fresh("?window=0")
        up = card(pg, "Update")
        expect(up.locator("p.state")).to_contain_text("To send new firmware: unplug the ice9, plug it back in, and send the file within 10 minutes.")
        expect(up.get_by_role("button", name="Send")).to_be_hidden()
        expect(up.locator("#upd-file")).to_be_hidden()
        pg.screenshot(path=f"{SHOTS}/{name}-update-shut.png", full_page=True)
        ctx.close()
    check(f"{name}: no password and window shut: no Send, how to open it", update_shut)

    # 5. The board refuses (window shut on the board after the page read it): its own words.
    def update_refused():
        ctx, pg = fresh("?window=600")
        up = card(pg, "Update")
        expect(up.get_by_role("button", name="Send")).to_be_visible()
        pg.evaluate("() => { const {m, save} = window.ice9Mock; m.plugged = 0; save(); }")
        pg.set_input_files("#upd-file", files=[FIRMWARE])
        up.get_by_role("button", name="Send").click()
        expect(up.locator("p.said")).to_contain_text("No permission: unplug the ice9, plug it back in, and send the update again within 10 minutes.")
        expect(up.locator("p.said")).to_have_class("said bad")
        expect(up.get_by_role("button", name="Send")).to_be_hidden(timeout=3000)
        ctx.close()
    check(f"{name}: a 403 shows the board's words, then the card shuts", update_refused)

    # 6. Password set, window shut: works; and a failure shows "Update Failed!".
    def update_locked_fail():
        ctx, pg = fresh("?locked&window=0&fail")
        up = card(pg, "Update")
        expect(up.locator("p.state")).to_contain_text("The page password lets new firmware in.")
        pg.set_input_files("#upd-file", files=[FIRMWARE])
        up.get_by_role("button", name="Send").click()
        expect(up.locator("p.said")).to_have_text("Update Failed!", timeout=8000)
        expect(up.get_by_role("button", name="Send")).to_be_enabled()
        ctx.close()
    check(f"{name}: password set, window shut: Send offered; 'Update Failed!' shown as the board's words", update_locked_fail)

    # 8. The wrong file: the factory image (even renamed) and something that isn't firmware are never sent.
    def wrong_files():
        ctx, pg = fresh("?locked&window=0")
        up = card(pg, "Update")
        expect(up.locator("label[for=upd-file]")).to_contain_text(".ota.bin")
        pg.evaluate("() => { const X = window.XMLHttpRequest; window.XMLHttpRequest = class extends X { send(f) { window.__sent = true; super.send(f); } }; }")
        pg.set_input_files("#upd-file", files=[FACTORY]); up.get_by_role("button", name="Send").click()
        expect(up.locator("p.said")).to_contain_text("That's the factory file")
        expect(up.locator("p.said")).to_contain_text("ice9-1.0.1.ota.bin")
        pg.set_input_files("#upd-file", files=[NOT_FIRMWARE]); up.get_by_role("button", name="Send").click()
        expect(up.locator("p.said")).to_contain_text("That isn't ice9 firmware.")
        assert not pg.evaluate("() => !!window.__sent"), "a wrong file was sent"
        expect(up.get_by_role("button", name="Send")).to_be_enabled()
        ctx.close()
    check(f"{name}: the factory image (renamed) and a non-firmware file are refused before sending", wrong_files)

    # 9. No "Can't reach the ice9" on an ordinary load; it does show when the stream is lost.
    def no_flash():
        ctx, pg = fresh()
        pg.goto(BASE)
        seen = pg.evaluate("""() => new Promise((done) => { const b = document.querySelectorAll('.banner')[1]; let shown = !b.hidden;
          const o = new MutationObserver(() => { if (!b.hidden) shown = true; }); o.observe(b, { attributes: true });
          setTimeout(() => { o.disconnect(); done(shown); }, 1500); })""")
        assert not seen, "the banner flashed on load"
        ctx.close()
    check(f"{name}: no 'Can't reach' flash on load", no_flash)

    # 10. The setup screen: list, pick, password checks, join, what to open next.
    def setup_screen():
        ctx, pg = fresh("?setup=empty")
        pg.goto(BASE + "?setup=empty")
        expect(pg.get_by_role("heading", name="Join it to your van's WiFi")).to_be_visible()
        expect(pg.locator(".view >> text=No WiFi heard yet")).to_be_visible()
        expect(pg.locator("button.net")).to_have_count(4, timeout=15000)      # filled in by itself; duplicate and hidden dropped
        names = pg.locator("button.net .net-name").all_inner_texts()
        assert names[0] == "My Van WiFi" and "" not in names and names.count("My Van WiFi") == 1, names
        expect(pg.locator("button.net", has_text="Campsite Guest").locator(".net-sig")).to_have_text("No password")
        expect(pg.locator("button.net .net-sig")).to_have_count(1)                    # only the open one is marked
        lines = pg.locator("button.net").all_inner_texts()
        assert not any(w in t for t in lines for w in ("Strong", "Good", "Weak")), lines   # no signal words (26 Nov)
        assert pg.locator("text=Settings").count() == 0 or pg.get_by_role("button", name="Settings").is_hidden()
        assert pg.locator("input[type=file]").is_hidden(), "an upload box on the setup screen"
        assert pg.locator(".banner").nth(1).is_hidden()
        pg.screenshot(path=f"{SHOTS}/{name}-setup-pick.png", full_page=True)
        pg.locator("button.net", has_text="My Van WiFi").click()
        expect(pg.get_by_role("heading", name="My Van WiFi")).to_be_visible()
        pg.fill("#wifi-pass", "short"); pg.get_by_role("button", name="Join").click()
        expect(pg.locator("#wifi-pass ~ p.said, .card p.said:visible").first).to_contain_text("8 to 63 characters")
        pg.fill("#wifi-pass", "correct horse battery"); pg.get_by_role("button", name="Join").click()
        expect(pg.get_by_role("heading", name="Nearly there")).to_be_visible()
        expect(pg.locator(".addr")).to_have_text("http://ice9-abcdef.local")
        expect(pg.locator("text=Put your phone back on")).to_contain_text("My Van WiFi")
        expect(pg.locator(".view >> text=Won't open? Try http://ice9-abcdef (without .local)")).to_contain_text("its IP address instead, for example http://192.168.1.50")
        saved = pg.evaluate("() => window.ice9Mock.saved")
        assert saved == {"ssid": "My Van WiFi", "psk": "correct horse battery"}, saved
        assert "correct horse battery" not in pg.content()
        all_from_page(pg, must=("/config.json", "/wifisave"))
        pg.screenshot(path=f"{SHOTS}/{name}-setup-done.png", full_page=True)
        ctx.close()
        # Not listed, and the answer lost as the box switches over.
        ctx, pg = fresh("?setup=dropped")
        pg.goto(BASE + "?setup=dropped")
        pg.get_by_role("button", name="It isn't listed").click()
        pg.get_by_role("button", name="Join").click()
        expect(pg.locator("#wifi-pass ~ p.said, .card p.said:visible").first).to_have_text("Type the WiFi's name first.")
        pg.fill("#wifi-name", "Hidden Network"); pg.fill("#wifi-pass", "hiddenpass1"); pg.get_by_role("button", name="Join").click()
        expect(pg.locator("#wifi-pass ~ p.said, .card p.said:visible").first).to_contain_text("No answer from the ice9")
        pg.get_by_role("button", name="Carry on").click()
        expect(pg.locator("text=Put your phone back on")).to_contain_text("Hidden Network")
        ctx.close()
    check(f"{name}: setup screen (list fills in, dedup, pick, checks, join, next steps; not listed; lost answer)", setup_screen)

    # 11. An ordinary load never shows the setup screen.
    def not_setup():
        ctx, pg = fresh()
        expect(card(pg, "Page password")).to_be_visible()
        assert pg.get_by_role("heading", name="Join it to your van's WiFi").is_hidden()
        ctx.close()
    check(f"{name}: no setup screen outside setup", not_setup)

    # 7. A 401 mid-session: reload once for the login box; refused again straight away: say so, don't loop.
    def mid_401():
        ctx, pg = fresh(answers=["other"])
        expect(card(pg, "Page password").locator("p.state")).to_contain_text("Off.")
        pg.evaluate("() => { const {m, save} = window.ice9Mock; m.cur = 'other'; save(); }")   # set from another phone
        with pg.expect_navigation(timeout=8000):
            pg.evaluate("() => { location.hash = ''; }")
            pg.get_by_role("button", name="Settings").click()                                    # any request: here, Settings reads /ice9/password
        expect(card(pg, "Page password").locator("p.state")).to_contain_text("On.", timeout=5000)
        assert len(pg.prompts) == 1, pg.prompts
        # Changed again from the other phone within a minute: no second reload.
        pg.evaluate("() => { const {m, save} = window.ice9Mock; m.cur = 'third'; save(); }")
        pg.evaluate("() => { location.hash = ''; }"); pg.get_by_role("button", name="Settings").click()
        expect(pg.locator("p.banner", has_text="couldn't log in with it")).to_be_visible(timeout=5000)
        url = pg.url; time.sleep(3)
        assert len(pg.prompts) == 1 and pg.url == url, "reloaded again"
        pg.screenshot(path=f"{SHOTS}/{name}-locked-out.png")
        ctx.close()
    check(f"{name}: 401 mid-session reloads once; a second within a minute says so and stops", mid_401)

    # The power limit (select/Adaptive Power). An FJX never reports one: no card.
    power = lambda pg: pg.locator("section.card", has=pg.locator(".label", has_text="Power limit"))
    def power_fjx():
        ctx, pg = fresh(screen="")
        expect(pg.locator(".status")).to_contain_text("Aircon SHE_a1b2c3", timeout=4000)   # the stream is up and has sent everything
        expect(pg.get_by_role("group", name="Fan speed")).to_be_visible()
        expect(power(pg)).to_be_hidden()
        assert pg.evaluate("() => document.body.innerText").count("Power limit") == 0
        ctx.close()
    check(f"{name}: an FJX (empty state): no power limit", power_fjx)

    # An FJZ: shows what the aircon says; a press shows as being set, not as set, until it confirms.
    def power_fjz():
        ctx, pg = fresh("?fjz", screen="")
        pc = power(pg); btn = lambda v: pc.get_by_role("button", name=v, exact=True)
        expect(pc).to_be_visible(timeout=4000)
        expect(btn("Unlimited")).to_have_attribute("aria-pressed", "true")
        expect(pc.locator("p.hint")).to_have_text("Caps how much current the aircon draws. Turn it down on a small hook-up or an inverter.")
        pg.screenshot(path=f"{SHOTS}/{name}-power-fjz.png", full_page=True)
        btn("5A").click()
        expect(btn("5A")).to_have_class("setting")
        expect(btn("5A")).to_have_attribute("aria-pressed", "false")
        expect(btn("Unlimited")).to_have_attribute("aria-pressed", "false")
        expect(pc.locator("p.said")).to_have_text("Setting 5A: waiting for the aircon to confirm it…")
        pg.screenshot(path=f"{SHOTS}/{name}-power-setting.png", full_page=True)
        expect(btn("5A")).to_have_attribute("aria-pressed", "true", timeout=4000)          # the mock's aircon confirms after 1.2 s
        expect(btn("5A")).not_to_have_class("setting")
        expect(pc.locator("p.said")).to_be_hidden()
        btn("5A").click()                                                                  # what it already is: nothing to wait for
        expect(pc.locator("p.said")).to_be_hidden()
        expect(btn("5A")).to_have_attribute("aria-pressed", "true")
        all_from_page(pg, must=("/select/Adaptive%20Power/set",))
        assert not pg.errors, pg.errors
        ctx.close()
    check(f"{name}: an FJZ: shown, pressed shows as being set, then confirmed", power_fjz)

    # An FJZ that doesn't confirm: back to the last confirmed value after about 5 s, and says so;
    # a confirmation that comes later still counts.
    def power_deaf():
        ctx, pg = fresh("?fjz=deaf", screen="")
        pc = power(pg); btn = lambda v: pc.get_by_role("button", name=v, exact=True)
        expect(btn("Unlimited")).to_have_attribute("aria-pressed", "true", timeout=4000)
        t0 = time.monotonic(); btn("6A").click()
        expect(btn("6A")).to_have_class("setting")
        expect(pc.locator("p.said")).to_have_text("The aircon didn't confirm 6A, so it's still on Unlimited. Try again.", timeout=8000)
        waited = time.monotonic() - t0
        assert 4.5 < waited < 6.5, f"went back after {waited:.1f} s"
        expect(pc.locator("p.said")).to_have_class("said bad")
        expect(btn("Unlimited")).to_have_attribute("aria-pressed", "true")
        expect(btn("6A")).not_to_have_class("setting")
        pg.screenshot(path=f"{SHOTS}/{name}-power-not-confirmed.png", full_page=True)
        pg.evaluate("() => window.ice9Mock.report('select/Adaptive Power', '6A')")
        expect(btn("6A")).to_have_attribute("aria-pressed", "true")
        expect(pc.locator("p.said")).to_be_hidden()
        assert not pg.errors, pg.errors
        ctx.close()
    check(f"{name}: an FJZ that doesn't confirm: back after 5 s and says so; a late confirmation still counts", power_deaf)

    # The Log (Settings): what the board keeps, new lines while open, the week, Copy.
    logc = lambda pg: card(pg, "Log")
    def log_card():
        ctx, pg = fresh(screen="")
        expect(pg.locator(".status")).to_contain_text("Aircon SHE_a1b2c3", timeout=4000)
        # Commands from the controls: the board logs who asked, from the header.
        pg.get_by_role("group", name="Mode").get_by_role("button", name="Heat").click()
        pg.get_by_role("group", name="Fan speed").get_by_role("button", name="Low").click()
        pg.get_by_role("switch", name="Interior Light").click()
        pg.wait_for_timeout(600)
        pg.get_by_role("button", name="Settings").click()
        lc = logc(pg); box = lc.locator(".log")
        expect(box).to_contain_text("Booted: power on", timeout=4000)
        expect(box.locator(".dayset > div").first).to_contain_text("Booted: power on")            # how it started, first
        expect(box).to_contain_text("Not asked by the ice9: its own panel, remote or app")
        expect(box).to_contain_text("Heat, 21°, fan Auto. Asked by: the page")
        expect(box).to_contain_text("Heat, 21°, fan Low. Asked by: the page")
        expect(box).to_contain_text("Inside light on. Asked by: the page")
        expect(lc.locator(".weekrow p.hint")).to_have_text("Since the ice9 last started. It keeps about a week as well.")
        expect(lc.locator("p.warn")).to_be_hidden()
        pg.screenshot(path=f"{SHOTS}/{name}-log.png", full_page=True)
        # A new line while it's open: added once, at the bottom.
        pg.evaluate("() => window.ice9Mock.record('Cool, 19°, fan High. Asked by: the Shortcut')")
        expect(box.locator(".dayset > div").last).to_have_text(__import__("re").compile(r"^\d\d:\d\d Cool, 19°, fan High\. Asked by: the Shortcut$"), timeout=8000)
        pg.wait_for_timeout(5500)                                                           # another read: nothing doubled
        texts = box.locator(".dayset > div").all_inner_texts()
        assert sum("Cool, 19°, fan High. Asked by: the Shortcut" in t for t in texts) == 1, texts
        assert sum("Booted:" in t for t in texts) == 1, texts
        # Copy: the clipboard where the browser has one (127.0.0.1 counts as secure)...
        pg.evaluate("() => { navigator.clipboard.writeText = async (t) => { window.__copied = t; }; }")
        lc.get_by_role("button", name="Copy the log").click()
        expect(lc.get_by_role("button", name="Copy the log")).to_have_text("Copied")
        got = pg.evaluate("() => window.__copied")
        head = got.splitlines()
        assert head[0].startswith("ice9 log since it last started, taken "), head[:3]
        assert head[1] == "Serial: abcdef. Firmware: 1.0.0-dev. Aircon: Connected to SHE_a1b2c3. Victron: Off (no broker set).", head[1]
        assert "Heat, 21°, fan Low. Asked by: the page" in got and "Booted: power on" in head[3], head[:5]
        # ...and select-and-copy where it hasn't (http://ice9-xxxxxx.local, as on a real box).
        pg.evaluate("""() => { Object.defineProperty(window, "isSecureContext", { value: false });
          document.execCommand = (c) => { window.__copied2 = c === "copy" ? window.getSelection().toString() || document.activeElement.value : ""; return true; }; }""")
        pg.wait_for_timeout(2600)
        lc.get_by_role("button", name="Copy the log").click()
        expect(lc.get_by_role("button", name="Copy the log")).to_have_text("Copied")
        got2 = pg.evaluate("() => window.__copied2")
        assert "Heat, 21°, fan Low. Asked by: the page" in got2 and got2.startswith("ice9 log since"), got2[:200]
        assert not pg.locator("textarea").count(), "the copy box was left in the page"
        # The week: read once, when asked; headings for each day; back again.
        lc.get_by_role("button", name="Show the last week").click()
        expect(lc.locator(".weekrow .btn")).to_have_text("Reading the week…")                # renamed while it reads
        expect(lc.locator(".weekrow p.hint")).to_contain_text("The last week, as the ice9 keeps it:", timeout=5000)
        expect(box.locator(".day")).to_have_count(5)
        expect(box).to_contain_text("Heat, 20°, fan Low. Asked by: the page")
        expect(lc.locator("p.hint", has_text="Copy and")).to_contain_text("the whole week")
        pg.screenshot(path=f"{SHOTS}/{name}-log-week.png", full_page=True)
        lc.get_by_role("button", name="Copy the log").click()
        expect(lc.get_by_role("button", name="Copy the log")).to_have_text("Copied")
        assert pg.evaluate("() => window.__copied2").startswith("ice9 log, the last week as the ice9 keeps it (")
        n = len(pg.evaluate("() => window.ice9Mock.asked.filter((a) => a[0] === '/ice9/log/all')"))
        assert n == 1, f"the week was read {n} times"
        pg.wait_for_timeout(5500)
        texts = box.locator(".dayset > div").all_inner_texts()
        assert sum("Cool, 19°, fan High. Asked by: the Shortcut" in t for t in texts) == 1, "a line the week had was added again"
        lc.get_by_role("button", name="Only since it last started").click()
        expect(box.locator(".day")).to_have_count(0)
        expect(lc.get_by_role("button", name="Show the last week")).to_be_visible()
        # Leaving Settings stops the reading.
        pg.get_by_role("button", name="‹ Back to the controls").click()
        before = len(pg.evaluate("() => window.ice9Mock.asked.filter((a) => a[0] === '/ice9/log')"))
        pg.wait_for_timeout(6000)
        after = len(pg.evaluate("() => window.ice9Mock.asked.filter((a) => a[0] === '/ice9/log')"))
        assert after == before, f"still reading the log off Settings ({after - before} more)"
        all_from_page(pg, must=("/climate/AC/set", "/select/AC%20fan%20speed/set", "/light/Interior%20Light/turn_on", "/ice9/log", "/ice9/log/all"))
        assert not pg.errors, pg.errors
        ctx.close()
    check(f"{name}: log: who asked, new lines once, Copy (clipboard and select-and-copy), the week once, stops off Settings", log_card)

    def log_old_and_odd():
        ctx, pg = fresh("?nolog")
        lc = logc(pg)
        expect(lc.locator("p.hint", has_text="keeps no log")).to_have_text("This ice9's firmware keeps no log. Newer firmware does: see Update, above.", timeout=4000)
        expect(lc.get_by_role("button", name="Show the last week")).to_be_hidden()
        expect(lc.get_by_role("button", name="Copy the log")).to_be_disabled()
        expect(lc.locator(".log")).to_have_text("Nothing in the log yet.")
        ctx.close()
        ctx, pg = fresh("?nohistory")
        lc = logc(pg)
        expect(lc.locator(".log")).to_contain_text("Booted: power on", timeout=4000)
        lc.get_by_role("button", name="Show the last week").click()
        expect(lc.locator(".weekrow p.hint")).to_have_text("This ice9's firmware keeps only the log since it last started.", timeout=4000)
        expect(lc.get_by_role("button", name="Show the last week")).to_be_hidden()
        expect(lc.locator(".log")).to_contain_text("Booted: power on")
        ctx.close()
        ctx, pg = fresh("?noflash&crash")
        lc = logc(pg)
        expect(lc.locator("p.warn")).to_have_text(__import__("re").compile(r"^The ice9's last start wasn't a clean one\. .*Booted: CRASH \(panic\)$"), timeout=4000)
        expect(lc.locator(".log .alert")).to_have_count(1)
        lc.get_by_role("button", name="Show the last week").click()
        expect(lc.locator(".weekrow p.hint")).to_contain_text("The ice9 has no week to show", timeout=4000)
        pg.screenshot(path=f"{SHOTS}/{name}-log-crash.png", full_page=True)
        assert not pg.errors, pg.errors
        ctx.close()
    check(f"{name}: log on old firmware (no log, no week), no flash for the week, a crash start", log_old_and_odd)

with sync_playwright() as p:
    for kind in ("chromium", "webkit"):
        b = getattr(p, kind).launch()
        run(b, kind)
        b.close()
srv.shutdown()
for r in results: print(*r)
sys.exit(any(r[0] == "FAIL" for r in results))
