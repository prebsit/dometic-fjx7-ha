// ice9: the box's own page. Loaded by ESPHome's web_server as /0.js (js_include) in place of the
// stock interface. It talks to the board's own web API, which is unchanged:
//   GET  /events                         live state, one JSON object per change (and all of it on connect)
//   POST /<domain>/<entity name>/<action>  e.g. /climate/AC/set?target_temperature=21
// and to components/ice9_lock (the top of its __init__.py has the contract):
//   GET/POST /ice9/password   the page password, and the plug-in window for firmware uploads
//   GET  /ice9/about          name, serial, firmware version
//   POST /update              a firmware file (ESPHome's web_server OTA, behind ice9_lock's gate)
// The entity names below are the `name:` values in packages/core.yaml and packages/webpage.yaml.
// If one is renamed there, rename it here.
const N = {
  ac: "climate/AC", fan: "select/AC fan speed", power: "select/Adaptive Power",
  inside: "light/Interior Light", outside: "light/Exterior Light",
  pairing: "text_sensor/Pairing", nearby: "text_sensor/AC units nearby", pairName: "text/AC name to pair with",
  host: "text/MQTT broker IP", user: "text/MQTT username", pass: "text/MQTT password", mqtt: "text_sensor/MQTT connection", passSaved: "binary_sensor/MQTT password saved",
  forget: "button/Forget AC (press twice within 10 s)", reset: "button/Factory reset (press twice within 10 s)", restart: "button/Restart",
};
const MODES = [["OFF", "Off"], ["COOL", "Cool"], ["HEAT", "Heat"], ["HEAT_COOL", "Auto"], ["FAN_ONLY", "Fan"], ["DRY", "Dry"]];
const FANS = ["Auto", "Low", "Medium", "High", "Turbo"];
const POWERS = ["4A", "5A", "6A", "7A", "Unlimited"];   // select/Adaptive Power's options, in order (packages/adaptive_power.yaml)
const MIN = 16, MAX = 31;

const s = {};            // latest state of every entity, keyed by id
let live = false;        // is the event stream up?
let lostOnce = false;    // has it failed, or not come up within 4 s? Until then there's nothing to warn about
let tempTimer = 0, tempWant = null;

const el = (tag, attrs = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "on") for (const [ev, fn] of Object.entries(v)) e.addEventListener(ev, fn);
    else if (k === "text") e.textContent = v;
    else if (v !== false && v != null) e.setAttribute(k, v === true ? "" : v);
  }
  e.append(...kids.filter((k) => k != null));
  return e;
};

// Every request the page makes goes through call(), so that a 401 (the ice9 has had a page
// password set since this page loaded, or it was changed from another phone) is noticed wherever
// it happens. The upload is the one exception (it needs XMLHttpRequest for its progress) and
// checks for a 401 itself.
// Each one says who's asking: X-Ice9-From: page (the Shortcut sends "shortcut"), so the board can
// put who asked for each command in its log. The upload sends it too. /events can't: a browser's
// EventSource takes no headers of its own, and it only listens.
const FROM = ["X-Ice9-From", "page"];
async function call(url, opts = {}) {
  const headers = new Headers(opts.headers); headers.set(...FROM);
  const r = await fetch(url, { ...opts, headers });
  if (r.status === 401) { lockedOut(); throw new Error("401"); }
  return r;
}
function send(id, action, params) {
  const [domain, ...name] = id.split("/");
  const q = params ? "?" + new URLSearchParams(params) : "";
  return call(`/${domain}/${encodeURIComponent(name.join("/"))}/${action}${q}`, { method: "POST" }).catch(() => {});
}
const val = (id) => { const o = s[id]; return o ? String(o.value ?? o.state ?? "") : ""; };

// ---------- page furniture ----------
const head = document.head;
head.append(el("meta", { name: "viewport", content: "width=device-width, initial-scale=1, viewport-fit=cover" }),
  el("meta", { name: "theme-color", content: "#f6f8f8" }));
document.title = "ice9";
document.documentElement.lang = "en";

const status = el("div", { class: "status", role: "status" });
const banner = el("div", { class: "banner", role: "alert", hidden: true, text: "Can't reach the ice9. Check your phone is on the van's WiFi. Trying again…" });

// ---------- the page password: logging in ----------
// The ice9 can have a password on its page (components/ice9_lock): HTTP Basic, any user name.
// The browser asks for it and remembers it; this page never sees it except while it is being
// typed into Settings, and keeps it nowhere.
// A 401 means this browser's login is missing or out of date. Reload, so the browser asks once,
// cleanly, instead of every background request failing on its own. Never twice in a row: if the
// page comes back and is refused again straight away, say so and stop.
const RELOADED = "ice9-reloaded-for-login";   // sessionStorage: when this tab last reloaded for a 401 (a time, never the password)
let locked = false, changingPw = false;
const lockLine = el("p", { class: "banner", role: "alert", hidden: true });
const session = { get: (k) => { try { return sessionStorage.getItem(k); } catch (_) { return null; } }, set: (k, v) => { try { sessionStorage.setItem(k, v); } catch (_) {} } };
function reloadForLogin(text, wait) {
  session.set(RELOADED, String(Date.now()));
  lockLine.textContent = text; lockLine.hidden = false;
  setTimeout(() => location.reload(), wait);
}
function lockedOut() {
  if (locked || changingPw) return;            // already handling it, or this page has just changed it
  locked = true; live = false;
  if (es) es.close();
  if (Date.now() - Number(session.get(RELOADED) || 0) < 60000) {
    lockLine.textContent = "This ice9 has a page password, and this page couldn't log in with it. Close this tab and open the ice9 again: it will ask for the password. The user name can be anything.";
    lockLine.hidden = false; render();
    return;
  }
  reloadForLogin("This ice9 has a page password now. Opening the page again, so you can type it in…", 1500);
  render();
}

// ---------- control screen ----------
const setTemp = el("div", { class: "temp-set", "aria-live": "polite" });
const now = el("div", { class: "now" });
const minus = el("button", { class: "round", type: "button", "aria-label": "Cooler by one degree", text: "−", on: { click: () => nudge(-1) } });
const plus = el("button", { class: "round", type: "button", "aria-label": "Warmer by one degree", text: "+", on: { click: () => nudge(1) } });
const tempCard = el("section", { class: "card temp", "aria-label": "Temperature" },
  el("div", { class: "temp-row" }, minus, setTemp, plus), now);

const seg = (label, items, onPick) => {
  const box = el("div", { class: "seg", role: "group", "aria-label": label });
  for (const [value, text] of items) {
    box.append(el("button", { type: "button", "data-v": value, "aria-pressed": "false", class: value === "OFF" ? "is-off" : null, text, on: { click: () => onPick(value) } }));
  }
  return box;
};
const mark = (box, value) => { for (const b of box.children) b.setAttribute("aria-pressed", String(b.dataset.v === value)); };

const modeSeg = seg("Mode", MODES, (m) => { patch(N.ac, { mode: m }); send(N.ac, "set", { mode: m }); });
const fanSeg = seg("Fan speed", FANS.map((f) => [f, f]), (f) => { patch(N.fan, { value: f, state: f }); send(N.fan, "set", { option: f }); });

// The power limit (FJZ): select/Adaptive Power. Its state is empty until the aircon reports a
// limit, and an FJX never does, so the card only shows when the state is one of the five.
// The board shows a new value only once the aircon has confirmed it (components/dometic_fjx7
// doesn't publish on a set), so unlike the fan, a press doesn't change the page's state: it shows
// that value as being set until the event stream brings it. Nothing within 5 s: back to what the
// aircon last said, and say so.
const POWER_WAIT = 5000;
let powerWant = null;     // pressed, not confirmed yet
let powerAsked = null;    // the last value pressed: a confirmation arriving after the 5 s still counts
let powerTimer = 0;
const powerSeg = seg("Power limit", POWERS.map((p) => [p, p]), pickPower);
const powerMsg = el("p", { class: "said", role: "status", hidden: true });
const powerSay = (text, bad) => { powerMsg.textContent = text; powerMsg.hidden = !text; powerMsg.classList.toggle("bad", !!bad); };
const powerCard = el("section", { class: "card", hidden: true }, el("div", { class: "label", text: "Power limit" }), powerSeg,
  el("p", { class: "hint", text: "Caps how much current the aircon draws. Turn it down on a small hook-up or an inverter." }), powerMsg);
function pickPower(p) {
  clearTimeout(powerTimer);
  if (p === val(N.power)) {                         // what it already is: nothing to wait for
    if (powerWant) send(N.power, "set", { option: p });   // overtakes the one still on its way
    powerWant = powerAsked = null; powerSay(""); render();
    return;
  }
  powerWant = powerAsked = p;
  powerSay(`Setting ${p}: waiting for the aircon to confirm it…`); render();
  send(N.power, "set", { option: p });
  powerTimer = setTimeout(() => {
    powerWant = null;
    powerSay(`The aircon didn't confirm ${p}, so it's still on ${val(N.power)}. Try again.`, true); render();
  }, POWER_WAIT);
}
function powerHeard() {                             // a new state for select/Adaptive Power from the stream
  if (powerAsked && val(N.power) === powerAsked) { clearTimeout(powerTimer); powerWant = powerAsked = null; powerSay(""); }
}

const sw = (label, isOn, toggle) => {
  const b = el("button", { class: "switch", type: "button", role: "switch", "aria-checked": "false", "aria-label": label, on: { click: () => toggle(b.getAttribute("aria-checked") !== "true") } });
  b.sync = () => b.setAttribute("aria-checked", String(isOn()));
  return b;
};
const light = (id) => sw(id.split("/")[1], () => val(id) === "ON", (on) => { patch(id, { value: on ? "ON" : "OFF", state: on ? "ON" : "OFF" }); send(id, on ? "turn_on" : "turn_off"); });
const sleepSw = sw("Sleep mode", () => (s[N.ac]?.preset || "NONE") === "SLEEP", (on) => { const p = on ? "SLEEP" : "NONE"; patch(N.ac, { preset: p }); send(N.ac, "set", { preset: p }); });
const inSw = light(N.inside), outSw = light(N.outside);
const row = (text, control) => el("div", { class: "row" }, el("span", { text }), control);
const outRow = row("Outside light", outSw);

const control = el("main", { class: "view" },
  tempCard,
  el("section", { class: "card" }, el("div", { class: "label", text: "Mode" }), modeSeg),
  el("section", { class: "card" }, el("div", { class: "label", text: "Fan" }), fanSeg),
  powerCard,
  el("section", { class: "card rows" }, row("Sleep mode", sleepSw), row("Inside light", inSw), outRow),
  el("div", { class: "foot" }, el("button", { class: "link", type: "button", text: "Settings", on: { click: () => go("config") } })));

function nudge(d) {
  const cur = tempWant ?? Math.round(parseFloat(s[N.ac]?.target_temperature));
  if (!Number.isFinite(cur)) return;
  tempWant = Math.min(MAX, Math.max(MIN, cur + d));
  render();
  clearTimeout(tempTimer);                       // several taps become one command
  tempTimer = setTimeout(() => { send(N.ac, "set", { target_temperature: tempWant }); patch(N.ac, { target_temperature: String(tempWant) }); tempWant = null; }, 600);
}

// ---------- settings screen ----------
const kv = (label) => { const b = el("b"); return [el("div", { class: "kv" }, el("span", { class: "label", text: label }), b), b]; };
const [pairKv, pairB] = kv("Aircon"), [nearKv, nearB] = kv("Aircons nearby"), [mqttKv, mqttB] = kv("Connection");
const field = (id, label, attrs = {}) => {
  const input = el("input", { class: "input", id, autocomplete: "off", autocapitalize: "off", spellcheck: "false", ...attrs });
  return [el("div", { class: "field" }, el("label", { class: "label", for: id, text: label }), input), input];
};
const [pairF, pairI] = field("pair-name", "Aircon name to pair with", { placeholder: "SHE_xxxxxx", maxlength: "16" });
const [hostF, hostI] = field("mqtt-host", "GX or broker address", { placeholder: "venus.local or 192.168.1.50", maxlength: "64", inputmode: "url" });
const [userF, userI] = field("mqtt-user", "Username (if it needs one)", { maxlength: "64" });
const [passF, passI] = field("mqtt-pass", "Password (if it needs one)", { maxlength: "64", type: "password" });
const dirty = new Set();
for (const i of [pairI, hostI, userI, passI]) i.addEventListener("input", () => dirty.add(i));

// The firmware wants two presses within 10 s for these; so does the page.
const twice = (text, id, armedText) => {
  let t = 0;
  const b = el("button", { class: "btn danger", type: "button", text });
  b.addEventListener("click", () => {
    send(id, "press");
    if (b.classList.toggle("armed")) { b.textContent = armedText; t = setTimeout(() => { b.classList.remove("armed"); b.textContent = text; }, 10000); }
    else { clearTimeout(t); b.textContent = text; }
  });
  return b;
};

// What Save did, said next to the button: the Connection line and the banner are further up the
// page, usually off the screen with the keyboard up.
const saveBtn = el("button", { class: "btn", type: "button", text: "Save", on: { click: saveMqtt } });
const mqttSay = el("p", { class: "said", role: "status", hidden: true });
const say = (text, bad) => { mqttSay.textContent = text; mqttSay.hidden = !text; mqttSay.classList.toggle("bad", !!bad); };
let saveTimer = 0;

// ---------- settings: the page password ----------
// GET  /ice9/password  {"set":false,"pending":false,"window":0}. Read each time Settings opens.
//      pending: a new password is on trial until someone logs in with it (10 minutes, then the
//      box goes back to how it was). window: seconds left of the plug-in window (see the update).
// POST /ice9/password  form fields: new (4-64 printable ASCII; empty removes it), and current when
//      one is set. 200 and the same JSON; 400 or 403 with a reason in plain text, shown as it comes.
const PW_MIN = 4, PW_MAX = 64;
let pw = null;          // what the ice9 last said, plus when: {set, pending, window, at}; null = not known yet
let pwOpen = "";        // the form showing: "" (none), "set", "change" or "remove"
let pwBusy = false;
const pwState = el("p", { class: "state" });
const pwField = (id, label, auto) => {
  const input = el("input", { class: "input", id, type: "password", autocomplete: auto, autocapitalize: "off", autocorrect: "off", spellcheck: "false", maxlength: String(PW_MAX) });
  input.addEventListener("input", () => pwSay(""));
  return [el("div", { class: "field" }, el("label", { class: "label", for: id, text: label }), input), input];
};
const [pwCurF, pwCur] = pwField("pw-current", "Current password", "current-password");
const [pwNewF, pwNew] = pwField("pw-new", "New password", "new-password");
const [pwAgainF, pwAgain] = pwField("pw-again", "The same again", "new-password");
const pwShow = sw("Show what I type", () => pwCur.type === "text", (on) => {
  for (const i of [pwCur, pwNew, pwAgain]) i.type = on ? "text" : "password";
  pwShow.sync();
});
const pwMsg = el("p", { class: "said", role: "status", hidden: true });
const pwSay = (text, bad) => { pwMsg.textContent = text; pwMsg.hidden = !text; pwMsg.classList.toggle("bad", !!bad); };
const pwGo = el("button", { class: "btn", type: "button", on: { click: savePw } });
const pwForm = el("div", { class: "form", hidden: true }, pwCurF, pwNewF, pwAgainF, row("Show what I type", pwShow),
  el("div", { class: "btns" }, pwGo, el("button", { class: "btn quiet", type: "button", text: "Cancel", on: { click: () => openPw("") } })));
const pwSetBtn = el("button", { class: "btn", type: "button", text: "Set a password", on: { click: () => openPw("set") } });
const pwChangeBtn = el("button", { class: "btn quiet", type: "button", text: "Change", on: { click: () => openPw("change") } });
const pwRemoveBtn = el("button", { class: "btn danger", type: "button", text: "Remove", on: { click: () => openPw("remove") } });
const pwCard = el("section", { class: "card" }, el("h2", { text: "Page password" }), pwState,
  el("div", { class: "btns" }, pwSetBtn, pwChangeBtn, pwRemoveBtn), pwForm, pwMsg,
  el("p", { class: "hint", text: "Home Assistant and the Victron screen don't use this page, so they carry on as they are. The iPhone shortcut does use it: give the shortcut the password too." }),
  el("p", { class: "hint", text: "Forgotten it? There's no way round it from a phone. Start the ice9 again from scratch with your hands on it: unplug it and plug it back in 5 times, unplugging within 10 seconds each time, and leave it plugged in the 5th time. That wipes the WiFi, the aircon pairing and the Victron settings as well as the password." }));

function openPw(which) {
  pwOpen = which;
  for (const i of [pwCur, pwNew, pwAgain]) { i.value = ""; i.type = "password"; }
  pwShow.sync(); pwSay(""); drawPw();
  const first = which === "set" ? pwNew : which ? pwCur : null;
  if (first) first.focus();
}
function drawPw() {
  const on = !!pw && pw.set;
  pwState.textContent = !pw ? "Asking the ice9…"
    : !on ? "Off. Anyone on the van's WiFi can use this page: work the aircon, change the settings, restart the ice9 or wipe it."
    : pw.pending ? "On, but the new password isn't kept yet: nobody has logged in with it. If nobody does within 10 minutes of it being set, or the ice9 loses power, it goes back to how it was before."
    : "On. The ice9 asks for it before it shows this page, on any phone. The user name can be anything: only the password counts.";
  pwSetBtn.hidden = !pw || on || !!pwOpen; pwChangeBtn.hidden = pwRemoveBtn.hidden = !on || !!pwOpen;
  pwForm.hidden = !pwOpen;
  pwCurF.hidden = pwOpen === "set"; pwNewF.hidden = pwAgainF.hidden = pwOpen === "remove";
  pwGo.textContent = pwOpen === "remove" ? "Remove the password" : pwOpen === "change" ? "Change it" : "Set it";
  pwGo.className = pwOpen === "remove" ? "btn danger" : "btn";
  pwGo.disabled = pwBusy;
}
const took = (o) => ({ set: !!o.set, pending: !!o.pending, window: Math.max(0, Number(o.window) || 0), at: Date.now() });
async function readPw() {
  try {
    const r = await call("/ice9/password", { cache: "no-store" });
    if (r.ok) pw = took(await r.json());
  } catch (_) { /* unreachable, or a 401 (handled): leave it as it was */ }
  drawPw(); drawUpd();
  if (!pw) pwState.textContent = "Couldn't ask the ice9 about its password. Open Settings again to try again.";
}
// What the board accepts (components/ice9_lock): 4 to 64 characters, printable ASCII only.
const pwBad = (p) => (p.length < PW_MIN || p.length > PW_MAX ? `The password must be ${PW_MIN} to ${PW_MAX} characters.`
  : /[^\x20-\x7e]/.test(p) ? "Letters, numbers, spaces and ordinary punctuation only: no accents or emoji." : "");
async function savePw() {
  if (pwBusy) return;
  const form = new URLSearchParams();
  if (pwOpen !== "set") {
    if (!pwCur.value) { pwSay("Type the current password first.", true); pwCur.focus(); return; }
    form.set("current", pwCur.value);
  }
  if (pwOpen === "remove") form.set("new", "");
  else {
    const bad = pwBad(pwNew.value);
    if (bad) { pwSay(bad, true); pwNew.focus(); return; }
    if (pwNew.value !== pwAgain.value) { pwSay("The two new passwords aren't the same.", true); pwAgain.focus(); return; }
    form.set("new", pwNew.value);
  }
  pwBusy = true; drawPw();
  let r = null, why = "";
  try { r = await call("/ice9/password", { method: "POST", body: form, cache: "no-store" }); why = r.ok ? "" : (await r.text()).trim(); }
  catch (e) { if (e.message === "401") { pwBusy = false; return; } }
  pwBusy = false;
  for (const i of [pwCur, pwNew, pwAgain]) i.value = "";   // gone from the page either way
  if (!r) { pwSay("Couldn't reach the ice9. Nothing was changed. Try again.", true); drawPw(); return; }
  if (!r.ok) { pwSay(why || `The ice9 refused it (${r.status}).`, true); drawPw(); return; }
  let o = {}; try { o = await r.json(); } catch (_) {}
  const was = pwOpen;
  pw = took(o); pwOpen = ""; drawPw(); drawUpd();
  if (was === "remove") { pwSay("Removed. Anyone on the van's WiFi can use this page again."); return; }
  // The browser's remembered login is now missing or wrong. Open the page again, so it asks once.
  changingPw = true;
  pwSay("Saved. The ice9 will ask for the new password now. The user name can be anything.");
  reloadForLogin("The ice9 will ask for the new password now. The user name can be anything.", 2500);
}

// ---------- settings: update ----------
// POST /update  a multipart upload of the firmware file. Taken with the page password given (the
//      browser sends its login by itself), or with none set for `window` seconds after the box was
//      plugged in. Otherwise 403 with the board's reason in plain text. 200 "Update Successful!"
//      and the box restarts. ESPHome also answers 200 "Update Failed!..." when it couldn't take
//      the file, so the words decide, not the status.
// GET /ice9/about  {"name","serial","version","esphome","window"}: the version before and after.
let about = null;
// This ice9: what a customer reads out when they ask for help. The serial is the 6 characters
// printed on the case and the setup sheet (the end of its name, lower case, as the sheet prints it).
const [serialKv, serialB] = kv("Serial"), [verKv, verB] = kv("Firmware");
serialB.classList.add("mono");
let updating = false;   // from Send until the ice9 is back: the Update card says what's happening, so the banner doesn't
let sendingFile = false; // while the file is going: the board answers little else, so the stream isn't restarted
let updTimer = 0;
const updState = el("p", { class: "state" });
const updFile = el("input", { class: "input file", id: "upd-file", type: "file" });
const updFileF = el("div", { class: "field" }, el("label", { class: "label", for: "upd-file", text: "Firmware file: its name ends in .ota.bin" }), updFile);
const updBtn = el("button", { class: "btn", type: "button", text: "Send", on: { click: sendUpdate } });
const updMsg = el("p", { class: "said", role: "status", hidden: true });
const updSay = (text, bad) => { updMsg.textContent = text; updMsg.hidden = !text; updMsg.classList.toggle("bad", !!bad); };
const updSend = el("div", { class: "form" }, updFileF, updBtn);
const updCard = el("section", { class: "card" }, el("h2", { text: "Update" }), updState, updSend, updMsg);
updFile.addEventListener("change", () => updSay(""));

const windowLeft = () => (pw ? Math.max(0, Math.round(pw.window - (Date.now() - pw.at) / 1000)) : 0);
const mmss = (t) => `${Math.floor(t / 60)}:${String(t % 60).padStart(2, "0")}`;
function drawUpd() {
  serialB.textContent = about ? about.serial || "?" : "…";
  verB.textContent = about ? about.version || "?" : "…";
  const left = windowLeft(), open = !!pw && (pw.set || left > 0);
  updState.textContent = !pw ? "Asking the ice9…"
    : pw.set ? "The page password lets new firmware in. Choose the file, then Send."
    : left > 0 ? `The ice9 has just been plugged in, so it takes new firmware for another ${mmss(left)}. Choose the file, then Send.`
    : "To send new firmware: unplug the ice9, plug it back in, and send the file within 10 minutes. Or set a page password (above): then it takes new firmware at any time, from someone who knows the password.";
  updSend.hidden = !open && !updating;
  updFile.disabled = updBtn.disabled = updating;
}
async function readAbout() {
  try { const r = await call("/ice9/about", { cache: "no-store" }); if (r.ok) about = await r.json(); }
  catch (_) { /* unreachable, or a 401 (handled) */ }
  drawUpd();
  return about;
}
// The plug-in window counts down on the page; it shuts on the board at the same time.
setInterval(() => { if (!settings.hidden && pw && !pw.set && pw.window && !updating) drawUpd(); }, 1000);

// The upload takes the app image (firmware.ota.bin, which goes out as ice9-<version>.ota.bin). The
// factory image (firmware.factory.bin: bootloader, partition table and app, for flashing over USB)
// is refused here, by its contents rather than its name, the way ESPHome's own uploader tells them
// apart (esphome/ota_image.py, is_factory_image): a partition table at 0x8000 and an app at 0x10000.
// What the board would do with one has not been tried.
const ESP_IMAGE = 0xe9;
async function notFirmware(file) {
  const b = new Uint8Array(await file.slice(0, 0x10001).arrayBuffer());
  const factory = b.length > 0x10000 && b[0x8000] === 0xaa && b[0x8001] === 0x50 && b[0x10000] === ESP_IMAGE;
  const name = "Send the file whose name ends in .ota.bin (like ice9-1.0.1.ota.bin).";
  if (factory) return `That's the factory file, for setting up a new ice9 over USB, not an update. ${name}`;
  if (b[0] !== ESP_IMAGE) return `That isn't ice9 firmware. ${name}`;
  return "";
}

// XMLHttpRequest rather than fetch: it says how much of the file has gone.
const upload = (file, progress) => new Promise((done) => {
  const x = new XMLHttpRequest();
  x.open("POST", "/update");
  x.setRequestHeader(...FROM);
  x.upload.addEventListener("progress", (e) => { if (e.lengthComputable) progress(e.loaded / e.total); });
  x.addEventListener("load", () => done({ status: x.status, text: x.responseText }));
  for (const ev of ["error", "abort", "timeout"]) x.addEventListener(ev, () => done(null));
  const form = new FormData();
  form.append("update", file, file.name);
  x.send(form);
});
async function sendUpdate() {
  const file = updFile.files[0];
  if (updating) return;
  if (!file) { updSay("Choose the firmware file first.", true); return; }
  clearInterval(updTimer);
  updating = true; drawUpd(); render();
  const wrong = await notFirmware(file);
  if (wrong) { updating = false; drawUpd(); render(); updSay(wrong, true); return; }
  updSay("Starting…");
  const before = (await readAbout())?.version;
  if (locked) return;
  const keepOpen = "Keep this page open until it says it's back.";
  sendingFile = true;
  const r = await upload(file, (f) => updSay(f < 1 ? `Sending: ${Math.floor(f * 100)}%. ${keepOpen}` : `Sent. The ice9 is checking the file. ${keepOpen}`));
  sendingFile = false; heard = Date.now();
  if (r && r.status === 401) { updating = false; lockedOut(); return; }
  if (!r || r.status !== 200 || !/^Update Successful/.test(r.text)) {
    updating = false; render();
    if (!live) listen();                            // its stream may have dropped while the file was going
    updSay(r ? r.text.trim() || `The ice9 refused it (${r.status}).` : "The file didn't get to the ice9: the connection dropped. It's still on the firmware it had. Try again.", true);
    readPw();                                       // the plug-in window may have shut meanwhile
    return;
  }
  updFile.value = "";
  updSay("Sent. The ice9 is restarting: about 20 seconds.");
  const t0 = Date.now();
  let asked = false, reading = false;
  const done = (text, bad) => { clearInterval(updTimer); updating = false; heard = Date.now(); updSay(text, bad); readPw(); render(); };
  updTimer = setInterval(async () => {
    const t = Date.now() - t0;
    if (t < 8000) return;                           // the board restarts straight after it answers
    if (!asked) { asked = true; live = false; listen(); render(); return; }   // a new stream: Safari can sit on the dead one
    if (live && !reading) {
      reading = true;
      const now = (await readAbout())?.version;
      reading = false;
      if (now) {
        done(!before ? `Back, on firmware ${now}.` : now === before ? `Back, still on firmware ${now}: the file was the same version.` : `Back. Firmware ${before} before, ${now} now.`);
        return;
      }
    }
    if (t > 90000) done("Still can't reach the ice9. Check it has power, then reload this page.", true);
  }, 1000);
}

// ---------- settings: the log ----------
// What the ice9 has been asked to do, and by whom: to read in the van, and to send when
// asking for help. In F451's formats (f451 page/f451.js, "log"), so the same reading applies:
//   GET /ice9/log      since the ice9 last started: how it started ("Booted: ...", the first line,
//                      which stays put), then the recent lines, oldest first, a line each
//                      ("14:02 Cool, 21°, fan Auto. Asked by: the page"), and a heading
//                      "--- Thu 26 Nov ---" whenever the day changes. Read when Settings opens and
//                      every 5 s while it is open; only lines not seen before are added.
//   GET /ice9/log/all  about a week, kept through restarts and power cuts: one summary line
//                      ("History: 812 lines, oldest first (...)"), then the same lines and headings.
//                      Big, and the board is busy while it sends it, so it is read once, when
//                      someone asks ("Show the last week"), never on the timer.
//   An ice9 whose firmware has neither answers 404.
// No live lines from /events: packages/webpage.yaml has `log: false` (the live log would show the
// WiFi password during setup), so this only reads what the board keeps.
// The lines are shown, and copied, exactly as the board sends them.
const LOG_MAX = 800, LOG_POLL = 5000, WEEK_WAIT = 30000;
let logLines = [];             // { text, cls }: cls "" (a line), "day" (a day's heading), "alert" (a start nobody asked for)
let record = null;             // the board's lines as last read, to tell what is new; null = not read yet
let logReading = false, logTimer = 0, logHas = true;
let week = null;               // null: since the last start is showing. Else the week is: { count, from }
let weekReading = false, weekHas = true, weekSaid = "";
let weekTail = null;           // the week's last lines: the next read of /ice9/log drops any it already had
const logBox = el("div", { class: "log", role: "log", tabindex: "0", "aria-label": "The ice9's log" });
const logState = el("p", { class: "hint", role: "status" }), bootLine = el("p", { class: "hint warn", hidden: true });
const DAY = /^--- (.+) ---$/;
// A start nobody asked for: "Booted: CRASH (...)", "BROWNOUT: ..." or "watchdog".
const badStart = (text) => /\bBooted: .*(CRASH|BROWNOUT|watchdog)/i.test(text);
const lineOf = (text) => ({ text, cls: DAY.test(text) ? "day" : badStart(text) ? "alert" : "" });
const lineEl = (l) => l.cls === "day" ? el("div", { class: "day", role: "heading", "aria-level": "3", text: l.text.replace(DAY, "$1") }) : el("div", { class: l.cls || null, text: l.text });
// The same line in the week and in /ice9/log: the week cuts a long one short, ending "...".
const sameLine = (kept, line) => kept === line || (kept.endsWith("...") && line.startsWith(kept.slice(0, -3)));
const atEnd = () => logBox.scrollHeight - logBox.scrollTop - logBox.clientHeight < 48;
// Each day's lines in a box of their own, so its heading sticks only while that day is in view
// (all in one box, every heading sticks and the earlier ones show behind the later).
function drawLog(stick = true) {
  const days = [];
  for (const l of logLines) { if (l.cls === "day" || !days.length) days.push(el("div", { class: "dayset" })); days[days.length - 1].append(lineEl(l)); }
  logBox.replaceChildren(...days);
  if (!logBox.firstChild) logBox.append(el("div", { class: "empty", text: record == null && logHas ? "Reading the log…" : "Nothing in the log yet." }));
  if (stick) logBox.scrollTop = logBox.scrollHeight;
}
function addLines(list) {
  if (!list.length) return;
  const stick = atEnd();          // follow the newest line, unless the reader has scrolled up to look at something
  logLines.push(...list);
  const cap = (week ? week.count : 0) + LOG_MAX;
  if (logLines.length > cap) logLines = logLines.slice(-cap + 100);
  drawLog(stick);
}
async function readLog() {
  if (logReading || !logHas || weekReading || locked || updating || sendingFile || setupMode) return;
  logReading = true;
  try {
    const r = await call("/ice9/log", { cache: "no-store" });
    if (!r.ok) throw new Error(String(r.status));
    const now = (await r.text()).split(/\r?\n/).filter((t) => t.trim());
    // What is new: under its first line ("Booted: ...") the log only grows at the end and loses
    // lines from the start, so find where what we had runs into what there is now. A different
    // first line, or no overlap, means the ice9 has restarted (or a lot happened): all of it is new.
    let fresh = now;
    if (record) {
      const head = (a) => (a.length && /\bBooted: /.test(a[0]) ? 1 : 0), rh = head(record), nh = head(now);
      if (rh === nh && (!rh || record[0] === now[0])) {
        const was = record.slice(rh), is = now.slice(nh);
        if (!was.length) fresh = is;
        else for (let skip = 0; skip < was.length; skip++) {
          const keep = was.length - skip;
          if (keep <= is.length && was.slice(skip).every((t, i) => t === is[i])) { fresh = is.slice(keep); break; }
        }
      }
    }
    if (weekTail) { const tail = weekTail; weekTail = null; fresh = fresh.filter((t) => !tail.some((k) => sameLine(k, t))); }
    const first = record == null;
    record = now; logState.textContent = "";
    const starts = now.filter((t) => /\bBooted: /.test(t)), last = starts[starts.length - 1] || "";
    bootLine.hidden = !badStart(last); bootLine.textContent = badStart(last) ? `The ice9's last start wasn't a clean one. ${last}` : "";
    if (first) drawLog();
    addLines(fresh.map(lineOf));
    if (first) drawWeek();
  } catch (e) {
    if (e.message === "404") {
      logHas = false; record = [];
      logState.textContent = "This ice9's firmware keeps no log. Newer firmware does: see Update, above.";
      drawLog(); drawWeek();
    } else if (record == null && e.message !== "401") logState.textContent = "Couldn't read the log from the ice9. Trying again.";
  } finally { logReading = false; }
}
const weekBtn = el("button", { class: "btn quiet", type: "button", text: "Show the last week", on: { click: () => readWeek() } });
const weekBack = el("button", { class: "link", type: "button", text: "Only since it last started", on: { click: () => showSinceStart() } });
const weekLine = el("p", { class: "hint", "aria-live": "polite" });
const weekRow = el("div", { class: "weekrow" }, weekLine, weekBtn, weekBack);
function drawWeek() {
  weekRow.hidden = !logHas;
  weekBtn.hidden = !!week || !weekHas;
  weekBtn.disabled = weekReading || record == null;
  weekBtn.textContent = weekReading ? "Reading the week…" : "Show the last week";
  weekBack.hidden = !week;
  weekLine.textContent = weekSaid || (week ? `The last week, as the ice9 keeps it: ${week.count} lines${week.from ? `, from ${week.from}` : ""}. New lines are added at the bottom.`
    : "Since the ice9 last started. It keeps about a week as well.");
  weekLine.classList.toggle("warn", !!weekSaid && !week);
  copyWhat.textContent = week ? `Copy and ${shareBtn.textContent} give the whole week shown above.` : `Copy and ${shareBtn.textContent} give the log since the ice9 last started.`;
  copyBtn.disabled = shareBtn.disabled = !logLines.length;
}
async function readWeek() {
  if (weekReading || week) return;
  weekReading = true; weekSaid = ""; drawWeek();
  const stop = new AbortController(), timer = setTimeout(() => stop.abort(), WEEK_WAIT);
  try {
    const r = await call("/ice9/log/all", { cache: "no-store", signal: stop.signal });
    if (r.status === 404) { weekHas = false; weekSaid = "This ice9's firmware keeps only the log since it last started."; return; }
    if (!r.ok) throw new Error(String(r.status));
    const all = (await r.text()).split(/\r?\n/).filter((t) => t.trim());
    const head = all[0] || "";
    if (/^History: not available/i.test(head)) { weekSaid = "The ice9 has no week to show: its memory wouldn't keep it, so there is only the log since it last started."; return; }
    const lines = /^History: /.test(head) ? all.slice(1) : all;
    if (!lines.length) { weekSaid = "Nothing kept yet."; return; }
    const firstDay = lines.find((t) => DAY.test(t));
    week = { count: lines.length, from: firstDay ? firstDay.replace(DAY, "$1") : "" };
    weekTail = lines.filter((t) => !DAY.test(t)).slice(-40);
    logLines = lines.map(lineOf);
    drawLog();
  } catch (e) {
    if (e.message !== "401") weekSaid = e.name === "AbortError" ? "The ice9 took too long sending the week. Try again in a minute." : "Couldn't read the week from the ice9. Try again.";
  } finally {
    clearTimeout(timer); weekReading = false; drawWeek();
    if (!locked) readLog();
  }
}
function showSinceStart() {
  week = null; weekTail = null; weekSaid = "";
  logLines = (record || []).map(lineOf);
  drawLog(); drawWeek();
}
function logShown(on) {
  clearInterval(logTimer); logTimer = 0;
  if (!on || locked) return;
  readLog();
  logTimer = setInterval(readLog, LOG_POLL);
}
// What Copy and Share give: what it is, which ice9, then the lines as shown.
const logText = () => [week ? `ice9 log, the last week as the ice9 keeps it (${week.count} lines${week.from ? `, from ${week.from}` : ""}), taken ${new Date().toString()}` : `ice9 log since it last started, taken ${new Date().toString()}`,
  `Serial: ${about?.serial || "?"}. Firmware: ${about?.version || "?"}. Aircon: ${val(N.pairing) || "?"}. Victron: ${val(N.mqtt) || "?"}.`, "",
  ...logLines.map((l) => l.text)].join("\n");
// http://ice9-xxxxxx.local is not a "secure context", so the browser's clipboard and share calls are
// often missing there. Copy falls back to the old select-and-copy; Share falls back to a file.
async function copyText(text) {
  try { if (navigator.clipboard && window.isSecureContext) { await navigator.clipboard.writeText(text); return true; } } catch (_) {}
  const box = el("textarea", { class: "offscreen", readonly: true, "aria-hidden": "true" });
  box.value = text; document.body.append(box); box.focus(); box.select(); box.setSelectionRange(0, text.length);
  let ok = false; try { ok = document.execCommand("copy"); } catch (_) {}
  box.remove(); return ok;
}
const flash = (btn, text) => { const was = btn.dataset.text || btn.textContent; btn.dataset.text = was; btn.textContent = text; clearTimeout(btn.t); btn.t = setTimeout(() => { btn.textContent = was; }, 2500); };
const copyBtn = el("button", { class: "btn", type: "button", text: "Copy", "aria-label": "Copy the log", on: { click: async () => flash(copyBtn, (await copyText(logText())) ? "Copied" : "Couldn't copy") } });
const canShare = typeof navigator.share === "function";
const shareBtn = el("button", { class: "btn quiet", type: "button", text: canShare ? "Share" : "Save as a file", "aria-label": canShare ? "Share the log" : "Save the log as a file", on: { click: async () => {
  const text = logText();
  if (canShare) { try { await navigator.share({ title: week ? "ice9 log, the last week" : "ice9 log", text }); } catch (_) {} return; }
  const d = new Date(), p = (n) => String(n).padStart(2, "0");
  const a = el("a", { href: URL.createObjectURL(new Blob([text], { type: "text/plain" })), download: `ice9-${about?.serial || "log"}-${week ? "week" : "log"}-${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}.txt` });
  document.body.append(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 60000);
} } });
const copyWhat = el("p", { class: "hint" });
const logCard = el("section", { class: "card" }, el("h2", { text: "Log" }), bootLine, weekRow, logBox, logState,
  el("div", { class: "btns" }, copyBtn, shareBtn), copyWhat,
  el("p", { class: "hint", text: "Each line says what the aircon was asked to do, and who asked. \"Not asked by the ice9\" means someone used the aircon's own panel, its remote or its app." }),
  el("p", { class: "hint", text: "Asking for help? Copy the log and send it with your question." }));
drawLog(); drawWeek();

const settings = el("main", { class: "view", hidden: true },
  el("div", {}, el("button", { class: "link", type: "button", text: "‹ Back to the controls", on: { click: () => go("") } })),
  el("h1", { text: "Settings" }),
  el("section", { class: "card" }, el("h2", { text: "Aircon pairing" }), pairKv, nearKv,
    el("p", { class: "hint", text: "To pair: hold + and − on the aircon's panel for 3 seconds. If that doesn't work (FJX4), type its name here." }),
    pairF,
    el("div", { class: "btns" },
      el("button", { class: "btn", type: "button", text: "Pair", on: { click: () => { send(N.pairName, "set", { value: pairI.value }); dirty.delete(pairI); } } }),
      twice("Forget this aircon", N.forget, "Tap again to forget"))),
  el("section", { class: "card" }, el("h2", { text: "Victron GX or Node-RED" }), mqttKv, hostF, userF, passF,
    el("p", { class: "hint", text: "Saving restarts the ice9. It takes about 20 seconds. Leave the address blank to switch this off. A new address clears the saved password." }),
    saveBtn, mqttSay),
  pwCard, updCard,
  el("section", { class: "card" }, el("h2", { text: "This ice9" }),
    el("div", { class: "pair" }, serialKv, verKv),
    el("p", { class: "hint", text: "Asking for help (ice9.cool/help)? Quote the serial, and copy the log (below)." }),
    el("p", { class: "hint", text: "Starting again wipes the WiFi, the aircon pairing and the Victron settings. The ice9 comes back as its setup hotspot." }),
    el("div", { class: "btns" },
      el("button", { class: "btn quiet", type: "button", text: "Restart", on: { click: () => send(N.restart, "press") } }),
      twice("Start again from scratch", N.reset, "Tap again to wipe it"))),
  logCard);

async function saveMqtt() {
  // Only what changed: every change restarts the firmware's 5 s countdown.
  // The address first: a new address clears the saved password on the board (it belongs to the
  // GX it was typed for), so a password typed now has to arrive after it.
  const changed = [[hostI, N.host], [userI, N.user], [passI, N.pass]].filter(([input]) => dirty.has(input));
  const cleared = dirty.has(hostI) && !dirty.has(passI);
  clearInterval(saveTimer);
  if (!changed.length) { say("Nothing has changed, so there's nothing to save."); return; }
  saveBtn.disabled = true; say("Saving…");
  for (const [input, id] of changed) {
    const r = await send(id, "set", { value: input.value.trim() });
    if (locked) return;                                // a 401: the page is reloading for the login
    if (!r || !r.ok) { saveBtn.disabled = false; say("Couldn't reach the ice9. Try again.", true); return; }
    dirty.delete(input);
  }
  say("Saved. The ice9 is restarting: about 20 seconds.");
  patch(N.mqtt, { value: "", state: "" });          // only a report from after the restart counts below
  const off = !hostI.value.trim(), t0 = Date.now();
  let asked = false;
  const done = (text, bad) => { clearInterval(saveTimer); saveBtn.disabled = false; say(text, bad); };
  saveTimer = setInterval(() => {
    const t = Date.now() - t0;
    if (t < 8000) return;                           // the board restarts 5 s after the last change
    if (!asked) { asked = true; live = false; listen(); render(); return; }   // a new stream: Safari can sit on the dead one
    const m = val(N.mqtt);
    if (live && off && /^Off/.test(m)) done("Back. This is switched off.");
    else if (live && m === "Connected") done("Back, and connected.");
    else if (t > 60000) done(!live ? "Still can't reach the ice9. Check it has power, then reload this page."
      : cleared ? "Back, but not connected. A new address clears the saved password: if your GX needs one, type it again and save."
      : "Back, but not connected. Check the address, user name and password.", true);
  }, 1000);
}

// ---------- drawing ----------
function patch(id, change) { s[id] = { ...(s[id] || {}), ...change }; render(); }

function render() {
  const ac = s[N.ac] || {};
  const off = (ac.mode || "OFF") === "OFF";
  const t = tempWant ?? Math.round(parseFloat(ac.target_temperature));
  setTemp.replaceChildren(Number.isFinite(t) ? String(t) : "–", el("sup", { text: "°" }));
  tempCard.classList.toggle("off", off);
  minus.disabled = !Number.isFinite(t) || t <= MIN; plus.disabled = !Number.isFinite(t) || t >= MAX;
  const cur = parseFloat(ac.current_temperature);
  now.textContent = Number.isFinite(cur) ? `${Math.round(cur)}° in the van now` : "";
  mark(modeSeg, ac.mode || ""); mark(fanSeg, val(N.fan));
  const power = val(N.power);
  powerCard.hidden = !POWERS.includes(power);
  mark(powerSeg, powerWant ? "" : power);
  for (const b of powerSeg.children) { b.classList.toggle("setting", b.dataset.v === powerWant); b.toggleAttribute("aria-busy", b.dataset.v === powerWant); }
  for (const b of [sleepSw, inSw, outSw]) b.sync();
  outRow.hidden = !(N.outside in s);                  // some FJX4s have no outside light

  const pairing = val(N.pairing);
  pairB.textContent = pairing || "…"; nearB.textContent = val(N.nearby) || "…"; mqttB.textContent = val(N.mqtt) || "…";
  for (const [input, id] of [[pairI, N.pairName], [hostI, N.host], [userI, N.user], [passI, N.pass]]) {
    if (!dirty.has(input) && document.activeElement !== input) input.value = val(id);
  }
  // The board never sends the password itself, only whether one is saved.
  const saved = s[N.passSaved]; passI.placeholder = saved && (saved.value === true || saved.state === "ON") ? "********" : "";
  const connected = /^Connected to/.test(pairing);
  status.textContent = !live ? "" : connected ? pairing.replace("Connected to", "Aircon") : pairing ? "Aircon not connected" : "";
  status.classList.toggle("bad", live && !!pairing && !connected);
  banner.hidden = live || !lostOnce || locked || updating || setupMode;     // while it takes a file and restarts, the Update card says what's happening
}

// ---------- the setup screen ----------
// While the setup hotspot is up, ESPHome's captive portal answers two addresses this page uses
// (esphome/components/captive_portal/captive_portal.cpp, 2026.11.0):
//   GET /config.json  {"mac":"..","name":"ice9-abcdef","aps":[{},{"ssid":"..","rssi":-52,"lock":1},..]}:
//                     the networks the ice9 can hear (the first entry is always an empty {})
//   GET /wifisave?ssid=..&psk=..  200 "Saved. Connecting...", then it joins that WiFi and, once it
//                     has, its own WiFi goes. A wrong password: its own WiFi comes back after 90 s
//                     (packages/provisioning.yaml, ap_timeout), until it has joined one once.
// Only the portal answers /config.json, so asking for it is how the page knows it is in setup.
// The board side (serving this page instead of ESPHome's own while the hotspot is up) is
// SetupScreen in components/ice9_lock/page_lock.cpp.
let setupMode = false, boxName = "", nets = [], netPick = null, netPoll = 0;
const SSID_MAX = 32;
const netList = el("div", { class: "nets" });
const netState = el("p", { class: "hint", role: "status" });
const [otherF, otherI] = field("wifi-name", "Its name, exactly as it's written", { maxlength: String(SSID_MAX) });
const [pskF, pskI] = field("wifi-pass", "Its password", { type: "password", maxlength: "64", autocomplete: "off" });
const pskShow = sw("Show what I type", () => pskI.type === "text", (on) => { pskI.type = on ? "text" : "password"; pskShow.sync(); });
const pskShowRow = row("Show what I type", pskShow);
const joinH = el("h2");
const joinOpen = el("p", { class: "hint", text: "This WiFi has no password." });
const joinMsg = el("p", { class: "said", role: "status", hidden: true });
const joinSay = (text, bad) => { joinMsg.textContent = text; joinMsg.hidden = !text; joinMsg.classList.toggle("bad", !!bad); };
const joinBtn = el("button", { class: "btn", type: "button", text: "Join", on: { click: join } });
const carryOn = el("button", { class: "btn quiet", type: "button", text: "Carry on", hidden: true, on: { click: () => setupStep("done") } });
for (const i of [otherI, pskI]) i.addEventListener("input", () => joinSay(""));

const pickStep = el("div", { class: "view" },
  el("h1", { text: "Join it to your van's WiFi" }),
  el("p", { class: "lead", text: "Choose your van's WiFi and type its password. The ice9 joins it, and you use its web page from there." }),
  el("section", { class: "card" }, el("h2", { text: "Your van's WiFi" }), netList, netState,
    el("div", { class: "btns" },
      el("button", { class: "btn quiet", type: "button", text: "Look again", on: { click: () => readNets(true) } }),
      el("button", { class: "link", type: "button", text: "It isn't listed", on: { click: () => pickNet(null) } }))),
  el("p", { class: "hint", text: "Your van's own WiFi, not a campsite's: anyone on the same WiFi can reach the ice9." }));
const joinStep = el("div", { class: "view", hidden: true },
  el("div", {}, el("button", { class: "link", type: "button", text: "‹ Choose another WiFi", on: { click: () => setupStep("pick") } })),
  el("section", { class: "card" }, joinH, otherF, joinOpen, pskF, pskShowRow, el("div", { class: "btns" }, joinBtn, carryOn), joinMsg));
const doneName = el("b"), doneAddr = el("div", { class: "addr" }), doneElse = el("p", { class: "hint" }), doneBack = el("p", { class: "hint" });
const doneLead = el("p", { class: "lead" });
const doneStep = el("div", { class: "view", hidden: true },
  el("h1", { text: "Nearly there" }), doneLead,
  el("section", { class: "card" }, el("h2", { text: "Next" }),
    el("ol", { class: "steps" },
      el("li", {}, "Put your phone back on ", doneName, "."),
      el("li", {}, "Open its web page in Safari or Chrome:", doneAddr, doneElse),
      el("li", {}, "Add it to your phone's home screen, so it is one tap away."))),
  doneBack);
const setup = el("main", { class: "view", hidden: true }, pickStep, joinStep, doneStep);

function setupStep(step) {
  pickStep.hidden = step !== "pick"; joinStep.hidden = step !== "join"; doneStep.hidden = step !== "done";
  clearInterval(netPoll); netPoll = 0;
  if (step === "pick") { readNets(false); netPoll = setInterval(() => { if (!nets.length) readNets(false); }, 5000); }
  if (step === "done") {
    const ssid = netPick ? netPick.ssid : otherI.value;
    doneName.textContent = ssid;
    doneLead.textContent = `The ice9 is joining ${ssid}. Its own WiFi, ${boxName}, will disappear in a moment: that's it joining yours.`;
    doneAddr.textContent = `http://${boxName}.local`;
    doneElse.textContent = `Won't open? Try http://${boxName} (without .local). Or find ${boxName} in your router's list of devices and use its IP address instead, for example http://192.168.1.50.`;
    doneBack.textContent = `If ${boxName}'s own WiFi comes back within two minutes, the password didn't work. Join it again, and this setup page will open again.`;
  }
  window.scrollTo(0, 0);
}
// The networks the ice9 can hear: one line each (the strongest, if a name is heard more than once),
// the strongest first. No signal words (owners aren't there to test their WiFi); only whether
// it has no password, because then there's no password to type.
function takeNets(o) {
  boxName = o.name || boxName;
  const by = new Map();
  for (const a of o.aps || []) {
    if (!a || typeof a.ssid !== "string" || !a.ssid) continue;   // the empty {} first, and hidden networks
    const had = by.get(a.ssid), rssi = Number(a.rssi) || -100;
    if (!had || rssi > had.rssi) by.set(a.ssid, { ssid: a.ssid, rssi, lock: !!Number(a.lock) || (had && had.lock) });
  }
  nets = [...by.values()].sort((a, b) => b.rssi - a.rssi);
  netList.replaceChildren(...nets.map((n) => el("button", { class: "net", type: "button", on: { click: () => pickNet(n) } },
    el("span", { class: "net-name", text: n.ssid }),
    n.lock ? null : el("span", { class: "net-sig", text: "No password" }))));
  netState.textContent = nets.length ? "" : "No WiFi heard yet. The ice9 keeps listening: this list fills in by itself.";
}
async function readNets(asked) {
  if (asked) netState.textContent = "Looking…";
  try { const r = await call("/config.json", { cache: "no-store" }); if (r.ok) { takeNets(await r.json()); return; } } catch (_) {}
  netState.textContent = "Couldn't ask the ice9. Check your phone is still on its WiFi, then Look again.";
}
function pickNet(n) {
  netPick = n;
  joinH.textContent = n ? n.ssid : "A WiFi that isn't listed";
  otherF.hidden = !!n; joinOpen.hidden = !n || n.lock; pskF.hidden = pskShowRow.hidden = !!n && !n.lock;
  otherI.value = ""; pskI.value = ""; pskI.type = "password"; pskShow.sync();
  joinSay(""); carryOn.hidden = true; joinBtn.disabled = false;
  setupStep("join");
  (n ? (n.lock ? pskI : null) : otherI)?.focus();
}
async function join() {
  const ssid = netPick ? netPick.ssid : otherI.value;
  const psk = !netPick || netPick.lock ? pskI.value : "";
  if (!netPick && !ssid.trim()) { joinSay("Type the WiFi's name first.", true); otherI.focus(); return; }
  if (new TextEncoder().encode(ssid).length > SSID_MAX) { joinSay("A WiFi name is at most 32 characters.", true); otherI.focus(); return; }
  // WPA2: 8 to 63 characters, or 64 hex digits. Blank only for a WiFi with no password.
  if ((netPick && netPick.lock) || psk) {
    if (!(psk.length >= 8 && psk.length <= 63) && !/^[0-9a-fA-F]{64}$/.test(psk)) { joinSay("A WiFi password is 8 to 63 characters. Check it and try again.", true); pskI.focus(); return; }
  }
  joinBtn.disabled = true; joinSay("Sending…");
  let r = null;
  try { r = await call("/wifisave?" + new URLSearchParams({ ssid, psk }), { cache: "no-store" }); } catch (_) {}
  joinBtn.disabled = false;
  if (r && r.ok) { pskI.value = ""; setupStep("done"); return; }
  // The answer can be lost as the ice9 switches over: say both, and let them carry on.
  joinSay(r ? `The ice9 said no (${r.status}). Try again.`
    : `No answer from the ice9. If your phone has dropped off ${boxName}'s WiFi, the ice9 probably has the password and is joining yours: Carry on. If not, try again.`, true);
  carryOn.hidden = !!r;
}
// Is the setup hotspot up? Only its captive portal answers /config.json. A short wait: in normal
// use nothing answers it (404), and this mustn't hold up the page.
async function setupCheck() {
  const stop = new AbortController(), t = setTimeout(() => stop.abort(), 3000);
  let o = null;
  try { const r = await call("/config.json", { cache: "no-store", signal: stop.signal }); if (r.ok) o = await r.json(); } catch (_) {}
  clearTimeout(t);
  if (!o || !Array.isArray(o.aps)) return false;
  setupMode = true; document.title = "ice9 setup";
  control.hidden = settings.hidden = true; setup.hidden = false;
  takeNets(o); setupStep("pick"); render();
  return true;
}

function go(view) { location.hash = view; }
function route() {
  if (setupMode) return;                            // one screen only, whatever the address says
  const set = location.hash === "#config";
  control.hidden = set; settings.hidden = !set; window.scrollTo(0, 0);
  if (set && !locked && !updating) { readPw(); readAbout(); }
  logShown(set);
}
addEventListener("hashchange", route);

document.body.append(el("div", { class: "app" },
  el("header", { class: "top" }, el("button", { class: "brand", type: "button", "aria-label": "ice9: back to the controls", text: "ice9", on: { click: () => go("") } }), status), lockLine, banner, control, settings, setup));
route(); render();

// ---------- live state ----------
let es = null, retryTimer = 0, heard = Date.now();
function listen() {
  if (es) es.close();
  const me = es = new EventSource("/events");
  const alive = () => { heard = Date.now(); if (!live) { live = true; render(); } };
  me.addEventListener("open", alive);
  me.addEventListener("ping", alive);
  me.addEventListener("state", (e) => { try { const o = JSON.parse(e.data); if (o.id) { s[o.id] = { ...(s[o.id] || {}), ...o }; if (o.id === N.power) powerHeard(); heard = Date.now(); live = true; render(); } } catch (_) {} });
  me.addEventListener("error", () => {
    if (me !== es) return;
    live = false; lostOnce = true; render();
    // The board tells the browser to wait 30 s before trying again; after a restart it is back in 10.
    clearTimeout(retryTimer);
    const again = () => { retryTimer = setTimeout(() => { if (!live && !locked && !sendingFile) listen(); }, 4000); };
    // Refused outright (a 401 once the ice9 has a page password, or any other answer that isn't
    // the stream), the browser gives up for good. Ask the ice9 why with one small request: a 401
    // is handled in call().
    if (me.readyState === 2 && !locked) call("/ice9/password", { cache: "no-store" }).catch(() => {}).finally(again);
    else again();
  });
}
// The setup hotspot gets the setup screen and nothing else; otherwise, the live state.
setupCheck().then((inSetup) => {
  if (inSetup) return;
  listen();
  setTimeout(() => { if (!live) { lostOnce = true; render(); } }, 4000);
});
// The board sends a ping every 10 s. Nothing for 25 s means the stream is dead, whatever Safari thinks.
// Not while a firmware file is going: the board answers little else meanwhile.
setInterval(() => { if (setupMode || locked || sendingFile) return; if (Date.now() - heard > 25000) { heard = Date.now(); live = false; lostOnce = true; render(); listen(); } }, 5000);
