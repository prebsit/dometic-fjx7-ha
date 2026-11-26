#!/usr/bin/env python3
"""Build Aircon.shortcut: control the bridge from an iPhone, with no GX or app in the middle.

Usage:
    python3 shortcuts/build_aircon.py                                # placeholder address
    python3 shortcuts/build_aircon.py --host ice9-abcdef.local       # one board's address
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

The board's address is the shortcut's first Text box, under a Comment that says so (the
password's box has one too). Every request is built from it, so pointing the shortcut at
another board means editing that one box.
(An import question for the address would be neater, but it made the Shortcuts app loop
on "Add Shortcut" when we tried it, so it's left out.)

The page password (components/ice9_lock, off until the owner sets one on the ice9's page):
when it is set, everything the board's web server serves wants it, HTTP Basic with any user
name, this shortcut's requests included. The second box is the password. It is filled in
by an import question ("ice9 page password (leave blank if you haven't set one)"), as
F451's shortcut does (f451 a362153), and can be edited afterwards like the address. NOTE:
the import question is the thing that looped above; F451's has been built and signed but
not tried on a phone either. Every request sends
  Authorization: Basic <base64 of "ice9:" and the password>
whether or not the box is empty: a board with no password lets everything through
(ice9_lock, PageLock::admits). The Base64 Encode action is told not to break its output
into lines, or a long password's header would break. A board that wants a password and
doesn't get the right one answers 401 with text starting "No permission", which is never
in a JSON answer; the shortcut looks for it after its first request and says so instead
of going on with nothing.

Every request also says who's asking: X-Ice9-From: shortcut (the ice9's page sends
"page"), so the board can say in its log who asked for each command. It rides in the
same headers as the login, so it goes wherever the login goes.
"""
import argparse
import os
import plistlib
import uuid

PLACEHOLDER = "ice9-xxxxxx.local"
OBJ = "￼"  # Shortcuts' placeholder character for an inline variable
DV = "Dictionary Value"
CONTENT = "Contents of URL"
SET = "/climate/AC/set"
LOGIN = "Login"  # base64 of "ice9:" and the page password
DENIED = "No permission"  # how ice9_lock's 401 text starts: see the page password, above
NEEDS_PASSWORD = "This ice9 wants its page password: set it in this shortcut's settings"
PAIRING = "/text_sensor/Pairing"  # "Connected to SHE_xxxxxx" only while it is (packages/webpage.yaml);
CONNECTED = "Connected to"        # otherwise "Not paired: ...", "Looking for ...", "Pairing with ...", "Paired, connecting to ..."
FROM = ("X-Ice9-From", "shortcut")  # who's asking, on every request: the board logs it against each command (the page sends "page")


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


def astext(attachment):
    """The same variable, told to Shortcuts as Text (as F451's build_heater.py: the If action
    leaves its comparison box empty for a variable it can't type)."""
    out = {"Value": dict(attachment["Value"]), "WFSerializationType": attachment["WFSerializationType"]}
    out["Value"]["Aggrandizements"] = [
        {"Type": "WFCoercionVariableAggrandizement", "CoercionItemClass": "WFStringContentItem"}]
    return out


def act(ident, **params):
    return {"WFWorkflowActionIdentifier": "is.workflow.actions." + ident, "WFWorkflowActionParameters": params}


def build(host):
    H, PW, L, B = U(), U(), U(), U()

    def url(path):
        return text("http://", (H, "Text"), path)

    # On every request (see the top of this file): the page password's login, and who's asking.
    login = {"Value": {"WFDictionaryFieldValueItems": [
        {"WFItemType": 0, "WFKey": text("Authorization"), "WFValue": text("Basic ", (B, LOGIN))},
        {"WFItemType": 0, "WFKey": text(FROM[0]), "WFValue": text(FROM[1])}]},
        "WFSerializationType": "WFDictionaryFieldValue"}

    def get(path, out):
        return act("downloadurl", UUID=out, WFURL=url(path), WFHTTPMethod="GET", ShowHeaders=True, WFHTTPHeaders=login)

    def post(path, fields):
        items = [{"WFItemType": 0, "WFKey": text(k), "WFValue": v if isinstance(v, dict) else text(v)}
                 for k, v in fields]
        return act("downloadurl", WFURL=url(path), WFHTTPMethod="POST", WFHTTPBodyType="Form",
                   ShowHeaders=True, WFHTTPHeaders=login,
                   WFFormValues={"Value": {"WFDictionaryFieldValueItems": items},
                                 "WFSerializationType": "WFDictionaryFieldValue"})

    def when_contains(src, words, then, otherwise):
        """If the answer from <src> contains <words>: then. Otherwise: otherwise. (WFCondition 99, as F451.)"""
        g = U()
        return ([act("conditional", GroupingIdentifier=g, WFControlFlowMode=0, WFCondition=99,
                     WFInput={"Type": "Variable", "Variable": astext(var(src, CONTENT))},
                     WFConditionalActionString=words)]
                + then
                + [act("conditional", GroupingIdentifier=g, WFControlFlowMode=1)]
                + otherwise
                + [act("conditional", GroupingIdentifier=g, WFControlFlowMode=2)])

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

    # A Comment above each of the two boxes an owner may need to edit, saying which is which: both
    # are plain "Text" boxes otherwise (26 Nov 2026).
    A = [act("comment", WFCommentActionText="ADDRESS. The box below is your ice9's address: ice9- and the six "
                                            "characters from your setup sheet, then .local"),
         act("gettext", UUID=H, WFTextActionText=host),  # the board's address: edit this box
         act("comment", WFCommentActionText="PASSWORD. The box below is your ice9's page password. "
                                            "Leave it empty if you haven't set one."),
         act("gettext", UUID=PW, WFTextActionText=""),   # the page password: the import question fills it; edit this box
         # "ice9:" and the password, made as a Text of its own: Base64 Encode's Input takes one
         # variable, not text with a variable in it. Given the text directly it was left EMPTY, the
         # header went out as "Basic " and nothing, and a box with a password said no to the right
         # one (an iPhone, 25 Nov 2026).
         act("gettext", UUID=L, WFTextActionText=text("ice9:", (PW, "Text"))),
         act("base64encode", UUID=B, CustomOutputName=LOGIN, WFInput=var(L, "Text"),
             WFEncodeMode="Encode", WFBase64LineBreakMode="None")]
    # The import question fills the password's box: found by where it is, not counted by hand.
    pw_at = next(i for i, a in enumerate(A) if a["WFWorkflowActionParameters"].get("UUID") == PW)
    questions = [{"ActionIndex": pw_at, "Category": "Parameter", "ParameterKey": "WFTextActionText", "DefaultValue": "",
                  "Text": "ice9 page password (leave blank if you haven't set one)"}]

    # Current state, for the menu prompts.
    C, M, T, CT, F, P = U(), U(), U(), U(), U(), U()
    A.append(get("/climate/AC", C))
    # The board wants its page password and didn't get it: say so, and stop there.
    A += when_contains(C, DENIED, [act("alert", WFAlertActionTitle="Aircon", WFAlertActionMessage=NEEDS_PASSWORD,
                                       WFAlertActionCancelButtonShown=False), act("exit")], [])
    # No aircon connected: the climate entity's numbers are nan and its mode a default ("OFF · set
    # nan°", seen on an iPhone, 25 Nov). Say so, in the board's own words, and stop there.
    PR, PV = U(), U()
    A.append(get(PAIRING, PR))
    A += when_contains(PR, CONNECTED, [], [
        key(PR, "value", PV),
        act("alert", WFAlertActionTitle="Aircon", WFAlertActionCancelButtonShown=False,
            WFAlertActionMessage=text("The aircon isn't connected to the ice9. The ice9 says: ", (PV, DV))),
        act("exit")])
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
    PR2, PV2 = U(), U()
    A += when_contains(C2, DENIED,
                       [act("notification", WFNotificationActionTitle="Aircon", WFNotificationActionBody=NEEDS_PASSWORD)],
                       [get(PAIRING, PR2)] + when_contains(PR2, CONNECTED,
                           [key(C2, "mode", M2), key(C2, "target_temperature", T2), key(C2, "fan_mode", F2),
                            act("notification", WFNotificationActionTitle="Aircon",
                                WFNotificationActionBody=text("Now ", (M2, DV), " · set ", (T2, DV), "° · fan ", (F2, DV)))],
                           [key(PR2, "value", PV2),
                            act("notification", WFNotificationActionTitle="Aircon",
                                WFNotificationActionBody=text("The aircon isn't connected to the ice9. The ice9 says: ", (PV2, DV)))]))

    return {
        "WFWorkflowClientVersion": "2302.0.4",
        "WFWorkflowMinimumClientVersion": 900,
        "WFWorkflowMinimumClientVersionString": "900",
        "WFWorkflowIcon": {"WFWorkflowIconStartColor": 4282601983, "WFWorkflowIconGlyphNumber": 59511},
        "WFWorkflowImportQuestions": questions,
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
