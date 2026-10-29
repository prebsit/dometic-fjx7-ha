#!/usr/bin/env python3
"""Build Aircon.shortcut: control the bridge from an iPhone, with no GX or app in the middle.

Usage:
    python3 shortcuts/build_aircon.py                                # placeholder address
    python3 shortcuts/build_aircon.py --host dometic-ac-43960c.local # one board's address
    shortcuts sign -m anyone -i Aircon.shortcut -o signed/Aircon.shortcut   # on a Mac

iOS only imports signed shortcuts, and signing needs a Mac signed in to iCloud
(`shortcuts sign`). Keep the signed file to yourself and share an iCloud link instead.

The shortcut talks straight to the board's own web server (ESPHome web_server, the
same one that serves the board's page), so the phone must be on the van WiFi:
  GET  /climate/AC              JSON: mode, target_temperature, current_temperature,
                                fan_mode, preset
  POST /climate/AC/set          form fields: mode, target_temperature, fan_mode, preset
  GET  /light/<name>            JSON: state ON / OFF
  POST /light/<name>/toggle
Every POST carries a form body: ESPHome's ESP-IDF web server answers 411 to a POST
with no Content-Length (web_server_idf.cpp, request_post_handler). Form fields are
searched before URL parameters (search_query_sources), so the settings ride in the body.

The board's address is the shortcut's first action, a plain Text box. Every request
is built from it, so pointing the shortcut at another board means editing that one box.
(An import question would be neater, but it made the Shortcuts app loop on "Add
Shortcut" when we tried it, so it's left out.)
"""
import argparse
import os
import plistlib
import uuid

PLACEHOLDER = "dometic-ac-xxxxxx.local"
OBJ = "￼"  # Shortcuts' placeholder character for an inline variable
DV = "Dictionary Value"
CONTENT = "Contents of URL"
SET = "/climate/AC/set"


def U():
    return str(uuid.uuid4()).upper()


def ref(u, name):
    return {"OutputUUID": u, "Type": "ActionOutput", "OutputName": name}


def var(u, name):
    return {"Value": ref(u, name), "WFSerializationType": "WFTextTokenAttachment"}


def text(*parts):
    """Text with inline variables. parts: str, or (action uuid, output name)."""
    s, att = "", {}
    for p in parts:
        if isinstance(p, str):
            s += p
        else:
            att[f"{{{len(s.encode('utf-16-le')) // 2}, 1}}"] = ref(*p)
            s += OBJ
    v = {"string": s}
    if att:
        v["attachmentsByRange"] = att
    return {"Value": v, "WFSerializationType": "WFTextTokenString"}


def act(ident, **params):
    return {"WFWorkflowActionIdentifier": "is.workflow.actions." + ident, "WFWorkflowActionParameters": params}


def build(host):
    H = U()

    def url(path):
        return text("http://", (H, "Text"), path)

    def get(path, out):
        return act("downloadurl", UUID=out, WFURL=url(path), WFHTTPMethod="GET")

    def post(path, fields):
        items = [{"WFItemType": 0, "WFKey": text(k), "WFValue": v if isinstance(v, dict) else text(v)}
                 for k, v in fields]
        return act("downloadurl", WFURL=url(path), WFHTTPMethod="POST", WFHTTPBodyType="Form",
                   WFFormValues={"Value": {"WFDictionaryFieldValueItems": items},
                                 "WFSerializationType": "WFDictionaryFieldValue"})

    def key(src, k, out):
        return act("getvalueforkey", UUID=out, WFInput=var(src, CONTENT), WFDictionaryKey=k,
                   WFGetDictionaryValueType="Value")

    def menu(prompt, options):
        g = U()
        out = [act("choosefrommenu", GroupingIdentifier=g, WFControlFlowMode=0,
                   WFMenuItems=[t for t, _ in options], WFMenuPrompt=prompt)]
        for t, acts in options:
            out.append(act("choosefrommenu", GroupingIdentifier=g, WFControlFlowMode=1, WFMenuItemTitle=t))
            out += acts
        out.append(act("choosefrommenu", GroupingIdentifier=g, WFControlFlowMode=2))
        return out

    A = [act("gettext", UUID=H, WFTextActionText=host)]  # the board's address: edit this box

    # Current state, for the menu prompts.
    C, M, T, CT, F, P = U(), U(), U(), U(), U(), U()
    A.append(get("/climate/AC", C))
    A += [key(C, "mode", M), key(C, "target_temperature", T), key(C, "current_temperature", CT),
          key(C, "fan_mode", F), key(C, "preset", P)]

    mode_menu = menu(text("Mode is ", (M, DV)), [
        ("Cool", [post(SET, [("mode", "COOL")])]),
        ("Heat", [post(SET, [("mode", "HEAT")])]),
        ("Auto", [post(SET, [("mode", "HEAT_COOL")])]),
        ("Fan only", [post(SET, [("mode", "FAN_ONLY")])]),
        ("Dry", [post(SET, [("mode", "DRY")])]),
        ("Off", [post(SET, [("mode", "OFF")])]),
    ])

    N = U()
    temp_acts = [
        act("ask", UUID=N, WFInputType="Number", WFAskActionPrompt="Target temperature (16 to 31 °C)",
            WFAskActionDefaultAnswer=text((T, DV))),
        post(SET, [("target_temperature", text((N, "Provided Input")))]),
    ]

    fan_menu = menu(text("Fan is ", (F, DV)), [
        ("Auto", [post(SET, [("fan_mode", "AUTO")])]),
        ("Low", [post(SET, [("fan_mode", "LOW")])]),
        ("Medium", [post(SET, [("fan_mode", "MEDIUM")])]),
        ("High", [post(SET, [("fan_mode", "HIGH")])]),
        ("Turbo", [post(SET, [("fan_mode", "Turbo")])]),
    ])

    # The AC only accepts Sleep in Cool or Heat; elsewhere it quietly ignores it.
    sleep_menu = menu(text("Sleep is ", (P, DV), " (NONE = off)"), [
        ("Sleep on", [post(SET, [("preset", "SLEEP")])]),
        ("Sleep off", [post(SET, [("preset", "NONE")])]),
    ])

    LI, LE, SI, SE = U(), U(), U(), U()
    light_acts = [get("/light/Interior%20Light", LI), key(LI, "state", SI),
                  get("/light/Exterior%20Light", LE), key(LE, "state", SE)]
    light_acts += menu(text("Interior: ", (SI, DV), " · Exterior: ", (SE, DV)), [
        ("Toggle interior light", [post("/light/Interior%20Light/toggle", [("go", "1")])]),
        ("Toggle exterior light", [post("/light/Exterior%20Light/toggle", [("go", "1")])]),
    ])

    A += menu(text("AC ", (M, DV), " · set ", (T, DV), "° · cabin ", (CT, DV), "° · fan ", (F, DV)), [
        ("Mode", mode_menu),
        ("Temperature", temp_acts),
        ("Fan speed", fan_menu),
        ("Sleep", sleep_menu),
        ("Lights", light_acts),
        ("Turn off", [post(SET, [("mode", "OFF")])]),
    ])

    # Read it back: the board's 200 means "sent", not "the AC agreed".
    C2, M2, T2, F2 = U(), U(), U(), U()
    A.append(act("delay", WFDelayTime=2))
    A.append(get("/climate/AC", C2))
    A += [key(C2, "mode", M2), key(C2, "target_temperature", T2), key(C2, "fan_mode", F2)]
    A.append(act("notification", WFNotificationActionTitle="Aircon",
                 WFNotificationActionBody=text("Now ", (M2, DV), " · set ", (T2, DV), "° · fan ", (F2, DV))))

    return {
        "WFWorkflowClientVersion": "2302.0.4",
        "WFWorkflowMinimumClientVersion": 900,
        "WFWorkflowMinimumClientVersionString": "900",
        "WFWorkflowIcon": {"WFWorkflowIconStartColor": 4282601983, "WFWorkflowIconGlyphNumber": 59511},
        "WFWorkflowImportQuestions": [],
        "WFWorkflowTypes": [],
        "WFWorkflowInputContentItemClasses": [],
        "WFWorkflowActions": A,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=PLACEHOLDER,
                    help=f"the board's address, from its setup sheet or an IP (default {PLACEHOLDER})")
    ap.add_argument("-o", "--output", default="Aircon.shortcut")
    args = ap.parse_args()
    with open(args.output, "wb") as f:
        plistlib.dump(build(args.host), f, fmt=plistlib.FMT_BINARY)
    print(f"Wrote {args.output} for {args.host}. Sign it on a Mac before importing:")
    out_dir, name = os.path.split(args.output)
    signed = os.path.join(out_dir, "signed", name)
    print(f"  mkdir -p {os.path.dirname(signed)} && shortcuts sign -m anyone -i {args.output} -o {signed}")


if __name__ == "__main__":
    main()
