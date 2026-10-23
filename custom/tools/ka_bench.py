"""NE102 mqttka performance benchmark.

Measures, averaged over multiple rounds:
  1. Keepalive maintenance: wake->hb-PUBACK (device serial "hb acked in
     Xms") + hb inter-arrival intervals (MQTT side), collected across the
     whole run.
  2. Remote capture latency per AT+KACAP skip config (0/1/3/5/10 - the
     knob that changes AE settle time and thus image content/size):
     send->capture evt, capture evt->photo, send->photo, photo size,
     device-side capture ms and publish ms.

Outlier policy: rounds where the broker session died mid-flight (serial
shows "unacked" or a fresh "mqtt up" inside the round) are reported but
excluded from the averages.

Usage: python ka_bench.py [rounds_per_config]
Writes ka_bench_result.json + prints a table. Runs ~30 min.
"""
import json
import os
import sys
import threading
import time

import serial as pyserial
import paho.mqtt.client as mqtt

MAC = "433ba8"
CMD = "ne102/%s/cmd" % MAC
EVT = "ne102/%s/evt" % MAC
PHOTO = "ne102/%s/photo" % MAC
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 5
SKIPS = ([int(x) for x in sys.argv[2].split(",")]
         if len(sys.argv) > 2 else [0, 1, 3, 5, 10])
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "ka_bench_result.json")

lock = threading.Lock()
events = []            # (time, kind, info)
slines = []            # (time, text)
parked = threading.Event()

def now():
    return time.time()

def on_msg(c, u, m):
    try:
        d = json.loads(m.payload)
        ev = d.get("event")
    except Exception:
        ev = None
    with lock:
        if m.topic == PHOTO:
            events.append((now(), "photo", len(m.payload)))
        else:
            events.append((now(), "evt", ev))
    if ev in ("hb", "phb", "sleep"):
        if not parked.is_set():
            print("%s sync: %s" % (time.strftime("%H:%M:%S"), ev), flush=True)
        parked.set()

c = mqtt.Client(client_id="ne102_bench")
c.on_message = on_msg
c.connect("broker.emqx.io", 1883, 60)
c.subscribe([(EVT, 1), (PHOTO, 1)])
c.loop_start()

ser = pyserial.Serial("COM7", 921600, timeout=0.2)
def ser_rx():
    while True:
        try:
            line = ser.readline().decode("utf-8", "replace").rstrip()
        except Exception:
            return
        if line:
            with lock:
                slines.append((now(), line))
threading.Thread(target=ser_rx, daemon=True).start()

def at(cmd, wait=3.0):
    ser.write((cmd + "\r\n").encode())
    time.sleep(wait)

def wait_event(kind_pred, timeout, after=0.0):
    t_end = now() + timeout
    while now() < t_end:
        with lock:
            for t, kind, info in events:
                if t >= after and kind_pred(kind, info):
                    return t, info
        time.sleep(0.05)
    return None, None

def new_serial_since(t0):
    with lock:
        return [l for (t, l) in slines if t >= t0]

def is_evt(name):
    return lambda k, i: k == "evt" and i == name

hb_w2a = []           # device-side wake->ack ms
hb_evt_ts = []        # MQTT-side hb arrival times
results = {}          # skip -> list of round dicts

def setup_config(skip):
    parked.clear()
    c.publish(CMD, "wake", qos=1)
    wait_event(is_evt("awake"), 30)   # may already be awake - not fatal
    at("AT+KAGO", 6)                  # heal a dead session before KASLP
    at("AT+KACAP=%d" % skip)
    at("AT+KASLP", 5.0)
    time.sleep(2)

for skip in SKIPS:
    print("=== config skip=%d ===" % skip, flush=True)
    setup_config(skip)
    rounds = []
    for r in range(ROUNDS):
        parked.clear()
        ok = parked.wait(130)
        if not ok:
            print("round %d: no park sync, skipping" % r, flush=True)
            continue
        time.sleep(10)                       # let it actually park
        t0 = now()
        s0 = len(slines)
        c.publish(CMD, "capture", qos=1)
        t_cap, _ = wait_event(is_evt("capture"), 25, after=t0)
        t_ph, size = wait_event(lambda k, i: k == "photo", 30,
                                after=t_cap or t0)
        time.sleep(3)                        # let device-side prints flush
        sl = new_serial_since(t0)
        rd = {
            "round": r,
            "cap_s": (t_cap - t0) if t_cap else None,
            "ph_s": (t_ph - t0) if t_ph else None,
            "size": size,
            "dev_cap_ms": None,
            "dev_pub_ms": None,
            "outlier": False,
        }
        for l in sl:
            if "captured " in l and " in " in l:
                try:
                    rd["dev_cap_ms"] = int(l.split(" in ")[1].split("ms")[0])
                except Exception:
                    pass
            if "photo published in " in l:
                try:
                    rd["dev_pub_ms"] = int(l.split(" in ")[1].split("ms")[0])
                except Exception:
                    pass
            if "unacked" in l or "mqtt up" in l:
                rd["outlier"] = True
        rd["evt_to_ph"] = ((t_ph - t_cap)
                           if (t_ph and t_cap) else None)
        rounds.append(rd)
        print("  r%d: cap=%.2fs ph=%.2fs size=%s dev_cap=%s dev_pub=%s %s"
              % (r, rd["cap_s"] or -1, rd["ph_s"] or -1, rd["size"],
                 rd["dev_cap_ms"], rd["dev_pub_ms"],
                 "OUTLIER" if rd["outlier"] else ""), flush=True)
        with lock:
            hb_evt_ts.extend(t for (t, k, i) in events
                             if k == "evt" and i == "hb" and t >= t0)
    results[skip] = rounds

time.sleep(2)
with lock:
    for t, l in slines:
        if "hb acked in " in l:
            try:
                hb_w2a.append(int(l.split(" in ")[1].split("ms")[0]))
            except Exception:
                pass
    all_hb = [t for (t, k, i) in events if k == "evt" and i == "hb"]
c.loop_stop()
c.disconnect()

# ---- report ----
def avg(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None

print("\n===== benchmark summary (%d rounds per config) =====" % ROUNDS)
rows = []
for skip, rounds in results.items():
    ok = [r for r in rounds if not r["outlier"] and r["cap_s"]]
    rows.append({
        "skip": skip, "n": len(ok),
        "size_avg": avg([r["size"] for r in ok]),
        "send2cap_avg": avg([r["cap_s"] for r in ok]),
        "cap2ph_avg": avg([r["evt_to_ph"] for r in ok]),
        "send2ph_avg": avg([r["ph_s"] for r in ok]),
        "dev_cap_ms_avg": avg([r["dev_cap_ms"] for r in ok]),
        "dev_pub_ms_avg": avg([r["dev_pub_ms"] for r in ok]),
        "outliers": len(rounds) - len(ok),
    })
    print("skip=%-2d n=%d size=%7.0fB send->cap=%.2fs cap->ph=%.2fs "
          "send->ph=%.2fs dev_cap=%sms dev_pub=%sms outliers=%d"
          % (skip, rows[-1]["n"], rows[-1]["size_avg"] or 0,
             rows[-1]["send2cap_avg"] or 0, rows[-1]["cap2ph_avg"] or 0,
             rows[-1]["send2ph_avg"] or 0, rows[-1]["dev_cap_ms_avg"],
             rows[-1]["dev_pub_ms_avg"], rows[-1]["outliers"]))

iv = sorted(b - a for a, b in zip(all_hb, all_hb[1:]) if 5 < b - a < 200)
print("\nkeepalive: hb samples=%d w2a(avg/min/max)=%.0f/%.0f/%.0f ms; "
      "evt interval(avg/min/max)=%.1f/%.1f/%.1f s (n=%d)"
      % (len(hb_w2a), avg(hb_w2a) or 0, min(hb_w2a) if hb_w2a else 0,
         max(hb_w2a) if hb_w2a else 0,
         avg(iv) or 0, min(iv) if iv else 0, max(iv) if iv else 0, len(iv)))

# merge with a previous run (reruns of individual configs)
prev = {}
if os.path.exists(OUT):
    try:
        for r in json.load(open(OUT))["rows"]:
            prev[r["skip"]] = r
    except Exception:
        pass
for r in rows:
    prev[r["skip"]] = r
rows = sorted(prev.values(), key=lambda r: r["skip"])
with open(OUT, "w") as f:
    json.dump({"rows": rows, "hb_w2a": hb_w2a,
               "hb_intervals": iv}, f, indent=1)
print("saved %s" % OUT)
