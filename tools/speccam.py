#!/usr/bin/env python3
"""SpecCam laptop tool: talk to the S3 engine for bring-up, teaching and self-tests.

Usage:
  python3 speccam.py status
  python3 speccam.py frame [out.jpg]          capture a frame from the camera
  python3 speccam.py measure [x y w h]        stats of a region (current frame)
  python3 speccam.py locate [dark_on_light|light_on_dark]
  python3 speccam.py push recipe.json         upload + activate a recipe
  python3 speccam.py setref                   set recipe locate.ref from the current part position
  python3 speccam.py inspect                  capture + inspect once
  python3 speccam.py live on|off
  python3 speccam.py teach CHECK N            teach anomaly check from N good parts (Enter per part)
  python3 speccam.py collect DIR good|bad N   save N labelled frames into DIR/good or DIR/bad
  python3 speccam.py selftest DIR             run active recipe on DIR/good + DIR/bad, print precision/recall

Set SPECCAM=http://<ip> if speccam.local does not resolve.
"""
import base64, json, os, sys, time, urllib.request

BASE = os.environ.get("SPECCAM", "http://speccam.local")


def get(path, raw=False):
    with urllib.request.urlopen(BASE + path, timeout=15) as r:
        data = r.read()
    return data if raw else json.loads(data)


def post(path, body, ctype="application/json"):
    req = urllib.request.Request(BASE + path, data=body.encode() if isinstance(body, str) else body,
                                 headers={"Content-Type": ctype}, method="POST")
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def show(d):
    print(json.dumps(d, indent=2))


def selftest(folder):
    rows = []
    for label in ("good", "bad"):
        d = os.path.join(folder, label)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.lower().endswith((".jpg", ".jpeg")):
                continue
            b64 = base64.b64encode(open(os.path.join(d, name), "rb").read()).decode()
            r = post("/inspect_image", b64, "text/plain")
            pred = r.get("state")
            rows.append((label, name, pred, r.get("failed", "")))
            print(f"{label:4} {name:28} -> {pred:5} {r.get('failed', '')}")
    # "bad" is the positive class: we want to catch every bad part
    tp = sum(1 for l, _, p, _ in rows if l == "bad" and p == "fail")
    fn = sum(1 for l, _, p, _ in rows if l == "bad" and p != "fail")
    fp = sum(1 for l, _, p, _ in rows if l == "good" and p == "fail")
    tn = sum(1 for l, _, p, _ in rows if l == "good" and p != "fail")
    prec = tp / (tp + fp) if tp + fp else 0
    rec = tp / (tp + fn) if tp + fn else 0
    print(f"\nSamples {len(rows)}  TP {tp} FN {fn} FP {fp} TN {tn}")
    print(f"Precision {prec:.2f}  Recall (defects caught) {rec:.2f}  False rejects {fp}/{fp + tn}")


def main(a):
    if not a:
        print(__doc__)
        return
    cmd = a[0]
    if cmd == "status":
        show(get("/status"))
    elif cmd == "frame":
        out = a[1] if len(a) > 1 else "frame.jpg"
        open(out, "wb").write(get("/frame.jpg", raw=True))
        print("saved", out)
    elif cmd == "measure":
        q = "" if len(a) < 5 else f"?x={a[1]}&y={a[2]}&w={a[3]}&h={a[4]}"
        show(get("/measure" + q))
    elif cmd == "locate":
        show(get("/locate?mode=" + (a[1] if len(a) > 1 else "dark_on_light")))
    elif cmd == "push":
        show(post("/recipe?activate=1", open(a[1]).read()))
    elif cmd == "setref":
        rec = get("/recipe")
        get("/frame.jpg", raw=True)
        loc = get("/locate?mode=" + rec["locate"]["mode"])
        if not loc["found"]:
            sys.exit("no part found, place a good part first")
        rec["locate"]["ref"] = loc["bbox"][:2]
        show(post("/recipe", json.dumps(rec)))
        print("ref =", loc["bbox"][:2])
    elif cmd == "inspect":
        show(get("/inspect"))
    elif cmd == "live":
        show(get("/live?on=" + ("1" if a[1] == "on" else "0")))
    elif cmd == "teach":
        check, n = a[1], int(a[2])
        show(get("/teach/start?check=" + check))
        for i in range(n):
            input(f"Place GOOD part {i + 1}/{n} (move it a little each time), press Enter...")
            show(get("/teach/add"))
        show(get("/teach/finish"))
    elif cmd == "collect":
        folder, label, n = a[1], a[2], int(a[3])
        d = os.path.join(folder, label)
        os.makedirs(d, exist_ok=True)
        for i in range(n):
            input(f"Place {label.upper()} part {i + 1}/{n}, press Enter...")
            name = os.path.join(d, f"{label}_{int(time.time() * 1000)}.jpg")
            open(name, "wb").write(get("/frame.jpg", raw=True))
            print("saved", name)
    elif cmd == "selftest":
        selftest(a[1])
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
