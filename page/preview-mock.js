// Stand-in for the board, for looking at the page in a browser with no hardware.
// Load BEFORE ice9.js. It fakes /events and the POST endpoints with a made-up FreshJet.
(() => {
  const st = {
    "climate/AC": { mode: "COOL", target_temperature: "21", current_temperature: "24.6", fan_mode: "AUTO", preset: "NONE" },
    "select/AC fan speed": { value: "Auto", state: "Auto" },
    "light/Interior Light": { value: "OFF", state: "OFF" },
    "light/Exterior Light": { value: "OFF", state: "OFF" },
    "text_sensor/Pairing": { value: "Connected to SHE_d13af0", state: "Connected to SHE_d13af0" },
    "text_sensor/AC units nearby": { value: "SHE_d13af0 (strong)", state: "SHE_d13af0 (strong)" },
    "text_sensor/MQTT connection": { value: "Off (no broker set)", state: "Off (no broker set)" },
    "text/AC name to pair with": { value: "", state: "" },
    "text/MQTT broker IP": { value: "", state: "" }, "text/MQTT username": { value: "", state: "" }, "text/MQTT password": { value: "", state: "" },
  };
  let es = null;
  const emit = (id) => es && es.dispatchEvent(new MessageEvent("state", { data: JSON.stringify({ id, ...st[id] }) }));
  window.EventSource = class extends EventTarget {
    constructor() { super(); es = this; setTimeout(() => { this.dispatchEvent(new Event("open")); Object.keys(st).forEach(emit); }, 150); }
    close() {}
  };
  window.fetch = async (url) => {
    const u = new URL(url, "http://box"); const [, domain, name, action] = u.pathname.split("/").map(decodeURIComponent);
    const id = `${domain}/${name}`, q = u.searchParams, o = st[id];
    setTimeout(() => {
      if (domain === "climate") { for (const k of ["mode", "preset", "target_temperature"]) if (q.has(k)) o[k] = q.get(k); }
      else if (domain === "select") o.value = o.state = q.get("option");
      else if (domain === "light") o.value = o.state = action === "turn_on" ? "ON" : "OFF";
      else if (domain === "text") {
        o.value = o.state = q.get("value");
        if (name === "MQTT broker IP") { const m = st["text_sensor/MQTT connection"]; m.value = m.state = o.value ? "Connected" : "Off (no broker set)"; emit("text_sensor/MQTT connection"); }
      }
      if (o) emit(id);
    }, 250);
    return new Response("", { status: 200 });
  };
})();
