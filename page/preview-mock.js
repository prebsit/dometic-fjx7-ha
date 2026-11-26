// Stand-in for the board, for looking at the page in a browser with no hardware.
// Load BEFORE ice9.js. It fakes /events and the POST endpoints with a made-up FreshJet, and
// components/ice9_lock (the page password, /ice9/about, /update) as it behaves on a board.
//
// In the address:
//   ?locked     a box that already has a page password ("ice9pass"), and this browser logged in with it
//   ?window=N   seconds left of the plug-in window at load (default 600, as if just plugged in; 0 = shut)
//   ?fail       the box takes the firmware file but says "Update Failed!"
//   ?setup      the setup hotspot is up: /config.json and /wifisave answer, as ESPHome's captive portal does
//               (?setup=empty: no networks heard for the first two asks; ?setup=dropped: /wifisave's answer is lost)
//   ?fjz        the aircon is an FJZ: it reports its power limit (select/Adaptive Power, "Unlimited" to
//               start; ?fjz=6A to start at another) and confirms a new one after about a second.
//               ?fjz=deaf: it reports one but never confirms a change. Without ?fjz it's an FJX: the
//               select is there, as on every v1 box, with an empty state, as ESPHome sends one never set.
//   ?nolog      a box whose firmware has no log (GET /ice9/log and /ice9/log/all answer 404)
//   ?nohistory  a box with /ice9/log but no /ice9/log/all (404)   ?noflash: /ice9/log/all says "History: not available"
//   ?crash      the box's last start was a crash, so its log starts "Booted: CRASH (panic)"
// Every request the mock gets is noted in window.ice9Mock.asked as [path, X-Ice9-From] (null when
// the header wasn't sent), and every command goes into its log with "Asked by: the page" (or
// whoever the header says), as the board will (the log's format: page/ice9.js, "the log").
// The mock keeps its state (the password, what the "browser" has logged in with) in sessionStorage,
// so it lasts across the page's own reloads; a new tab starts clean.
//
// The browser's login box is played by window.prompt(): when the page loads, or a reload follows
// a 401, and the box wants a password this "browser" doesn't have, it asks.
(() => {
  const q = new URLSearchParams(location.search);
  const KEY = "ice9-mock";
  const load = () => { try { return JSON.parse(sessionStorage.getItem(KEY)) || null; } catch (_) { return null; } };
  const m = load() || {
    cur: q.has("locked") ? "ice9pass" : "", prev: "", trialAt: 0,   // as components/ice9_lock: cur_, prev_, trial_since
    login: q.has("locked") ? "ice9pass" : null,                     // what the browser has remembered (null = nothing)
    plugged: Date.now() - (600 - Number(q.get("window") ?? 600)) * 1000,   // when the box was "plugged in"
    restarted: false,                                               // since then, by an update: the window stays shut
    version: "1.0.0-dev", wrong: 0, wrongAt: 0, downUntil: 0,
  };
  const save = () => sessionStorage.setItem(KEY, JSON.stringify(m));
  window.ice9Mock = { m, save };                  // for a test to play another phone (set or change the password behind the page's back)
  const TRIAL = 10 * 60 * 1000;

  const expire = () => { if (m.trialAt && Date.now() - m.trialAt > TRIAL) { m.cur = m.prev; m.prev = ""; m.trialAt = 0; save(); } };
  const admits = (p) => {
    expire();
    if (!m.cur) return true;
    if (p === m.cur) { if (m.trialAt) { m.trialAt = 0; m.prev = ""; save(); } return true; }
    return false;                                 // on trial, only the new one gets in (bcb4137)
  };
  const windowLeft = () => (m.restarted ? 0 : Math.max(0, Math.round(600 - (Date.now() - m.plugged) / 1000)));
  const down = () => Date.now() < m.downUntil;
  const pwJson = () => JSON.stringify({ set: !!m.cur, pending: !!m.trialAt, window: windowLeft() });
  const REFUSED = "No permission: this ice9 has a page password.\n\nReload this page and type it in. The user name can be anything (or nothing): only the password counts.\n\nForgotten it? It can only be cleared with your hands on the ice9: see the setup guide, \"Starting again\".\n";
  const NO_UPLOAD = "No permission: unplug the ice9, plug it back in, and send the update again within 10 minutes.\n\nAn ice9 with no page password only takes new firmware just after it has been plugged in.\nWith a page password set (Settings) it takes it at any time, from someone who has given it.\n";

  // The browser's login box, at page load.
  if (!admits(m.login)) {
    const p = window.prompt("ice9 (the login box): this ice9 has a page password. Type it in.");
    if (p != null) m.login = p;
  }
  save();
  if (!admits(m.login)) {                         // cancelled, or wrong: the browser shows the refusal
    document.addEventListener("DOMContentLoaded", () => { document.body.textContent = REFUSED; document.body.style.whiteSpace = "pre-line"; });
    window.fetch = async () => new Response(REFUSED, { status: 401 });
    return;
  }

  const st = {
    "climate/AC": { mode: "COOL", target_temperature: "21", current_temperature: "24.6", fan_mode: "AUTO", preset: "NONE" },
    "select/AC fan speed": { value: "Auto", state: "Auto" },
    "select/Adaptive Power": (() => { const v = !q.has("fjz") ? "" : /^[4-7]A$/.test(q.get("fjz")) ? q.get("fjz") : "Unlimited"; return { value: v, state: v }; })(),
    "light/Interior Light": { value: "OFF", state: "OFF" },
    "light/Exterior Light": { value: "OFF", state: "OFF" },
    "text_sensor/Pairing": { value: "Connected to SHE_a1b2c3", state: "Connected to SHE_a1b2c3" },
    "text_sensor/AC units nearby": { value: "SHE_a1b2c3 (strong)", state: "SHE_a1b2c3 (strong)" },
    "text_sensor/MQTT connection": { value: "Off (no broker set)", state: "Off (no broker set)" },
    "binary_sensor/MQTT password saved": { value: false, state: "OFF" },
    "text/AC name to pair with": { value: "", state: "" },
    "text/MQTT broker IP": { value: "", state: "" }, "text/MQTT username": { value: "", state: "" }, "text/MQTT password": { value: "", state: "" },
  };
  let es = null, asks = 0;
  window.ice9Mock.asked = [];
  const WHO = { page: "the page", shortcut: "the Shortcut" };

  // The board's log (GET /ice9/log): "Booted: ..." first, with its date, then a line each, "HH:MM text",
  // and a heading "--- Thu 26 Nov ---" when the day changes. Every line also goes into the week
  // (GET /ice9/log/all), after a made-up few days before this start.
  const p2 = (n) => String(n).padStart(2, "0");
  const DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"], MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
  const dayHead = (d) => `--- ${DAYS[d.getDay()]} ${p2(d.getDate())} ${MONTHS[d.getMonth()]} ---`;
  const hm = (d) => `${p2(d.getHours())}:${p2(d.getMinutes())}`;
  const bootAt = new Date(Date.now() - 50 * 60000), bootText = q.has("crash") ? "Booted: CRASH (panic)" : "Booted: power on";
  const events = [`${DAYS[bootAt.getDay()]} ${p2(bootAt.getDate())} ${MONTHS[bootAt.getMonth()]} ${hm(bootAt)} ${bootText}`];
  const kept = [];                                   // [when, text], this start's part of the week
  let logDay = bootAt.toDateString();
  const record = (text, at = new Date()) => {
    if (at.toDateString() !== logDay) { logDay = at.toDateString(); events.push(dayHead(at)); }
    events.push(`${hm(at)} ${text}`);
    while (events.length > 101) events.splice(1, 1);
    kept.push([at, text]);
  };
  kept.push([bootAt, bootText]);
  record("Connected to the aircon SHE_a1b2c3", new Date(bootAt.getTime() + 20000));
  record("The aircon turned on. Not asked by the ice9: its own panel, remote or app", new Date(bootAt.getTime() + 9 * 60000));
  record("Cool, 21°, fan Auto. Asked by: the Shortcut", new Date(bootAt.getTime() + 31 * 60000));
  window.ice9Mock.record = record;                   // for a test to add lines (a long log, a new day)
  function weekText() {
    const before = [];
    for (let back = 4; back >= 1; back--) {
      const d = new Date(); d.setDate(d.getDate() - back);
      const at = (h, m) => { const t = new Date(d); t.setHours(h, m, 0, 0); return t; };
      before.push([at(7, 10), "The aircon turned on. Not asked by the ice9: its own panel, remote or app"],
        [at(7, 12), "Heat, 20°, fan Low. Asked by: the page"], [at(9, 40), "Off. Asked by: the Shortcut"],
        [at(18, 5), "Cool, 21°, fan Auto. Asked by: the page"], [at(23, 30), "Off. Asked by: the page"]);
    }
    const all = [...before, ...kept], lines = [];
    let day = "";
    for (const [when, text] of all) {
      if (when.toDateString() !== day) { day = when.toDateString(); lines.push(dayHead(when)); }
      lines.push(`${hm(when)} ${text.length > 140 ? text.slice(0, 137) + "..." : text}`);
    }
    return `History: ${all.length} lines, oldest first (room for 1200; then the oldest go)\n` + lines.join("\n") + "\n";
  }
  // What the aircon is doing now, in the log's words.
  const MODE_WORD = { OFF: "Off", COOL: "Cool", HEAT: "Heat", HEAT_COOL: "Auto", FAN_ONLY: "Fan", DRY: "Dry" };
  const acNow = () => { const a = st["climate/AC"]; return a.mode === "OFF" ? "Off" : `${MODE_WORD[a.mode] || a.mode}, ${Math.round(+a.target_temperature)}°, fan ${st["select/AC fan speed"].value}`; };
  const emit = (id) => es && es.dispatchEvent(new MessageEvent("state", { data: JSON.stringify({ id, ...st[id] }) }));
  // For a test to play the aircon reporting something by itself (e.g. a confirmation that comes late).
  window.ice9Mock.report = (id, value) => { st[id].value = st[id].state = value; emit(id); };
  window.EventSource = class extends EventTarget {
    constructor() {
      super(); es = this; this.readyState = 0;
      setTimeout(() => {
        if (down() || !admits(m.login)) { this.readyState = down() ? 0 : 2; this.dispatchEvent(new Event("error")); return; }
        this.readyState = 1; this.dispatchEvent(new Event("open")); Object.keys(st).forEach(emit);
        this.ping = setInterval(() => this.dispatchEvent(new MessageEvent("ping", { data: "{}" })), 10000);
      }, 150);
    }
    close() { this.readyState = 2; clearInterval(this.ping); }
  };
  // The box restarting (after an update): the stream drops and nothing answers for a while.
  const restart = (ms) => { m.downUntil = Date.now() + ms; save(); if (es) { es.readyState = 0; clearInterval(es.ping); es.dispatchEvent(new Event("error")); } };

  window.fetch = async (url, opts = {}) => {
    const u = new URL(url, location.href);
    const from = new Headers(opts.headers).get("X-Ice9-From");
    window.ice9Mock.asked.push([u.pathname, from]);
    if (down()) throw new TypeError("Load failed");
    if (!admits(m.login)) return new Response(REFUSED, { status: 401 });
    if (u.pathname === "/config.json") {
      // Only the captive portal answers this, so only while the hotspot is up.
      if (!q.has("setup")) return new Response("", { status: 404 });
      asks = (asks || 0) + 1;
      const heard = q.get("setup") === "empty" && asks <= 2 ? [] : [
        { ssid: "My Van WiFi", rssi: -48, lock: 1 }, { ssid: "Campsite Guest", rssi: -77, lock: 0 },
        { ssid: "My Van WiFi", rssi: -63, lock: 1 }, { ssid: "", rssi: -70, lock: 1 },
        { ssid: "The \"Other\" Van's WiFi", rssi: -81, lock: 1 }, { ssid: "Router-1234", rssi: -69, lock: 1 }];
      return new Response(JSON.stringify({ mac: "3C:84:27:AB:CD:EF", name: "ice9-abcdef", aps: [{}, ...heard] }), { status: 200 });
    }
    if (u.pathname === "/wifisave") {
      if (!q.has("setup")) return new Response("", { status: 404 });
      window.ice9Mock.saved = { ssid: u.searchParams.get("ssid"), psk: u.searchParams.get("psk") };
      if (q.get("setup") === "dropped") throw new TypeError("Load failed");
      return new Response("Saved. Connecting...", { status: 200 });
    }
    if (u.pathname === "/ice9/log") return q.has("nolog") ? new Response("", { status: 404 }) : new Response(events.join("\n") + "\n", { status: 200 });
    if (u.pathname === "/ice9/log/all") {
      if (q.has("nolog") || q.has("nohistory")) return new Response("", { status: 404 });
      await new Promise((r) => setTimeout(r, 1200));   // the board sends it in pieces: it takes a moment
      return new Response(q.has("noflash") ? "History: not available. The ice9's memory wouldn't take it, so the log is since it last started only: /ice9/log\n" : weekText(), { status: 200 });
    }
    if (u.pathname === "/ice9/about") {
      return new Response(JSON.stringify({ name: "ice9-abcdef", serial: "abcdef", version: m.version, esphome: "2026.11.0", window: windowLeft() }), { status: 200 });
    }
    if (u.pathname === "/ice9/password") {
      if ((opts.method || "GET") === "POST") {
        const f = new URLSearchParams(opts.body);
        if (!f.has("new")) return new Response("Send the new password as the form field new (empty to remove it).", { status: 400 });
        const p = f.get("new");
        if (p && (p.length < 4 || p.length > 64)) return new Response("The password must be 4 to 64 characters.", { status: 400 });
        if (p && /[^\x20-\x7e]/.test(p)) return new Response("Letters, numbers, spaces and ordinary punctuation only: no accents or emoji.", { status: 400 });
        if (m.cur) {
          if (m.wrong >= 5 && Date.now() - m.wrongAt < TRIAL) return new Response("Too many wrong tries: wait 10 minutes before trying again.", { status: 403 });
          const c = f.get("current") || "";
          if (c !== m.cur) {
            if (m.wrong >= 5) m.wrong = 0;
            m.wrong++; m.wrongAt = Date.now(); save();
            return new Response("That isn't the current password.", { status: 403 });
          }
          m.wrong = 0;
        }
        if (!p) { m.cur = ""; m.prev = ""; m.trialAt = 0; }
        else { if (!m.trialAt) m.prev = m.cur; m.cur = p; m.trialAt = Date.now(); }
        save();
      }
      return new Response(pwJson(), { status: 200 });
    }
    const [, domain, name, action] = u.pathname.split("/").map(decodeURIComponent);
    const id = `${domain}/${name}`, q2 = u.searchParams, o = st[id];
    if (id === "select/Adaptive Power") {
      // The board passes it to the aircon and shows nothing new until the aircon reports it back.
      // An FJX never does; an FJZ does after about a second (not ?fjz=deaf).
      const want = q2.get("option");
      record(`Power limit ${want}. Asked by: ${WHO[from] || "not said"}`);
      if (q.has("fjz") && q.get("fjz") !== "deaf") setTimeout(() => { o.value = o.state = want; emit(id); }, 1200);
      return new Response("", { status: 200 });
    }
    setTimeout(() => {
      if (!o) return;
      if (domain === "climate") { for (const k of ["mode", "preset", "target_temperature"]) if (q2.has(k)) o[k] = q2.get(k); }
      else if (domain === "select") o.value = o.state = q2.get("option");
      else if (domain === "light") o.value = o.state = action === "turn_on" ? "ON" : "OFF";
      else if (domain === "text") {
        o.value = o.state = q2.get("value");
        if (name === "MQTT broker IP") { const c = st["text_sensor/MQTT connection"]; c.value = c.state = o.value ? "Connected" : "Off (no broker set)"; emit("text_sensor/MQTT connection"); }
      }
      emit(id);
      const by = `Asked by: ${WHO[from] || "not said"}`;
      if (domain === "climate" || id === "select/AC fan speed") record(`${acNow()}${q2.get("preset") === "SLEEP" ? ", sleep on" : q2.get("preset") === "NONE" ? ", sleep off" : ""}. ${by}`);
      else if (domain === "light") record(`${name === "Interior Light" ? "Inside" : "Outside"} light ${o.value === "ON" ? "on" : "off"}. ${by}`);
    }, 250);
    return new Response("", { status: 200 });
  };

  // POST /update: the file "goes" over about 3 seconds, then the box answers as ice9_lock and
  // ESPHome's web_server OTA would.
  window.XMLHttpRequest = class extends EventTarget {
    constructor() { super(); this.upload = new EventTarget(); this.status = 0; this.responseText = ""; this.headers = {}; }
    open(method, url) { this.url = url; }
    setRequestHeader(k, v) { this.headers[k.toLowerCase()] = v; }
    send(form) {
      window.ice9Mock.asked.push([this.url, this.headers["x-ice9-from"] ?? null]);
      const file = form && form.get("update"), total = (file && file.size) || 1;
      const answer = (status, text) => { this.status = status; this.responseText = text; this.dispatchEvent(new Event("load")); };
      if (down()) { setTimeout(() => this.dispatchEvent(new Event("error")), 300); return; }
      if (!admits(m.login)) { setTimeout(() => answer(401, REFUSED), 200); return; }
      if (!m.cur && windowLeft() === 0) { setTimeout(() => answer(403, NO_UPLOAD), 200); return; }
      let sent = 0;
      const tick = setInterval(() => {
        sent = Math.min(total, sent + total / 12);
        this.upload.dispatchEvent(new ProgressEvent("progress", { lengthComputable: true, loaded: sent, total }));
        if (sent < total) return;
        clearInterval(tick);
        setTimeout(() => {
          if (q.has("fail")) { answer(200, "Update Failed!"); return; }
          m.version = "1.0.1-mock"; m.restarted = true; save();
          answer(200, "Update Successful!");
          setTimeout(() => restart(6000), 300);
        }, 800);
      }, 250);
    }
  };
})();
