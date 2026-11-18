// Ice9: the box's own page. Loaded by ESPHome's web_server as /0.js (js_include) in place of the
// stock interface. It talks to the board's own web API, which is unchanged:
//   GET  /events                         live state, one JSON object per change (and all of it on connect)
//   POST /<domain>/<entity name>/<action>  e.g. /climate/AC/set?target_temperature=21
// The entity names below are the `name:` values in packages/core.yaml and packages/webpage.yaml.
// If one is renamed there, rename it here.
const N = {
  ac: "climate/AC", fan: "select/AC fan speed",
  inside: "light/Interior Light", outside: "light/Exterior Light",
  pairing: "text_sensor/Pairing", nearby: "text_sensor/AC units nearby", pairName: "text/AC name to pair with",
  host: "text/MQTT broker IP", user: "text/MQTT username", pass: "text/MQTT password", mqtt: "text_sensor/MQTT connection",
  forget: "button/Forget AC (press twice within 10 s)", reset: "button/Factory reset (press twice within 10 s)", restart: "button/Restart",
};
const MODES = [["OFF", "Off"], ["COOL", "Cool"], ["HEAT", "Heat"], ["HEAT_COOL", "Auto"], ["FAN_ONLY", "Fan"], ["DRY", "Dry"]];
const FANS = ["Auto", "Low", "Medium", "High", "Turbo"];
const MIN = 16, MAX = 31;

const s = {};            // latest state of every entity, keyed by id
let live = false;        // is the event stream up?
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

function send(id, action, params) {
  const [domain, ...name] = id.split("/");
  const q = params ? "?" + new URLSearchParams(params) : "";
  return fetch(`/${domain}/${encodeURIComponent(name.join("/"))}/${action}${q}`, { method: "POST" }).catch(() => {});
}
const val = (id) => { const o = s[id]; return o ? String(o.value ?? o.state ?? "") : ""; };

// ---------- page furniture ----------
const head = document.head;
head.append(el("meta", { name: "viewport", content: "width=device-width, initial-scale=1, viewport-fit=cover" }),
  el("meta", { name: "theme-color", content: "#f6f8f8" }));
document.title = "Ice9";
document.documentElement.lang = "en";

const status = el("div", { class: "status", role: "status" });
const banner = el("div", { class: "banner", role: "alert", hidden: true, text: "Can't reach the Ice9. Check your phone is on the van's WiFi. Trying again…" });

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
    el("p", { class: "hint", text: "Saving restarts the Ice9. It takes about 20 seconds. Leave the address blank to switch this off." }),
    el("button", { class: "btn", type: "button", text: "Save", on: { click: saveMqtt } })),
  el("section", { class: "card" }, el("h2", { text: "This Ice9" }),
    el("p", { class: "hint", text: "Starting again wipes the WiFi, the aircon pairing and the Victron settings. The Ice9 comes back as its setup hotspot." }),
    el("div", { class: "btns" },
      el("button", { class: "btn quiet", type: "button", text: "Restart", on: { click: () => send(N.restart, "press") } }),
      twice("Start again from scratch", N.reset, "Tap again to wipe it"))));

async function saveMqtt() {
  // Only what changed: every change restarts the firmware's 5 s countdown.
  for (const [input, id] of [[userI, N.user], [passI, N.pass], [hostI, N.host]]) {
    if (dirty.has(input)) { await send(id, "set", { value: input.value.trim() }); dirty.delete(input); }
  }
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
  for (const b of [sleepSw, inSw, outSw]) b.sync();
  outRow.hidden = !(N.outside in s);                  // some FJX4s have no outside light

  const pairing = val(N.pairing);
  pairB.textContent = pairing || "…"; nearB.textContent = val(N.nearby) || "…"; mqttB.textContent = val(N.mqtt) || "…";
  for (const [input, id] of [[pairI, N.pairName], [hostI, N.host], [userI, N.user], [passI, N.pass]]) {
    if (!dirty.has(input) && document.activeElement !== input) input.value = val(id);
  }
  const connected = /^Connected to/.test(pairing);
  status.textContent = !live ? "" : connected ? pairing.replace("Connected to", "Aircon") : pairing ? "Aircon not connected" : "";
  status.classList.toggle("bad", live && !!pairing && !connected);
  banner.hidden = live;
}

function go(view) { location.hash = view; }
function route() {
  const set = location.hash === "#config";
  control.hidden = set; settings.hidden = !set; window.scrollTo(0, 0);
}
addEventListener("hashchange", route);

document.body.append(el("div", { class: "app" },
  el("header", { class: "top" }, el("div", { class: "brand", text: "ICE9" }), status), banner, control, settings));
route(); render();

// ---------- live state ----------
function listen() {
  const es = new EventSource("/events");
  es.addEventListener("open", () => { live = true; render(); });
  es.addEventListener("state", (e) => { try { const o = JSON.parse(e.data); if (o.id) { s[o.id] = { ...(s[o.id] || {}), ...o }; live = true; render(); } } catch (_) {} });
  es.addEventListener("error", () => { live = false; render(); });   // the browser retries by itself
}
listen();
