"""Verify the capture-wake event sequence on the FIXED firmware:
wait for the device to park (sleep event), send 'capture', then record
every evt/photo message for 60s.

Expected (user ruling 2026-10-16): capture -> PHOTO -> sleep
No 'awake', no 'hb' between the capture command and the re-sleep 'sleep'.
"""
import json
import threading
import time

import paho.mqtt.client as mqtt

MAC = "433ba8"
CMD = "ne102/%s/cmd" % MAC
EVT = "ne102/%s/evt" % MAC
PHOTO = "ne102/%s/photo" % MAC

# serial tap: forward device-side MQTTKA prints (COM7) while probing
try:
    import serial as pyserial
    ser = pyserial.Serial("COM7", 921600, timeout=0.2)
    def _ser_rx():
        while True:
            try:
                line = ser.readline().decode("utf-8", "replace").rstrip()
            except Exception as ex:
                print("    [uart] TAP DEAD: %s" % ex, flush=True)
                return
            if line:
                print("    [uart] %s" % line, flush=True)
    threading.Thread(target=_ser_rx, daemon=True).start()
except Exception as e:
    print("(serial tap off: %s)" % e, flush=True)

events = []
lock = threading.Lock()
parked = threading.Event()

def stamp():
    return time.strftime("%H:%M:%S")

def on_evt(c, u, m):
    try:
        d = json.loads(m.payload)
        ev = d.get("event")
    except Exception:
        ev = "<raw %dB>" % len(m.payload)
    with lock:
        events.append((time.time(), "evt", ev))
    if ev in ("hb", "phb", "sleep"):   # hb: device just woke, will re-park
        if not parked.is_set():
            print("%s sync trigger: %s" % (stamp(), ev), flush=True)
        parked.set()

def on_photo(c, u, m):
    with lock:
        events.append((time.time(), "photo", len(m.payload)))

c = mqtt.Client(client_id="ne102_seq_probe")
c.on_message = on_evt
message_handlers = {EVT: on_evt, PHOTO: on_photo}
def _on_msg(cl, u, m):
    message_handlers.get(m.topic, lambda *a: None)(cl, u, m)
c.on_message = _on_msg
c.connect("broker.emqx.io", 1883, 60)
c.subscribe([(EVT, 1), (PHOTO, 1)])
c.loop_start()

print("waiting for park (sleep event), max 130s ...", flush=True)
if not parked.wait(130):
    print("TIMEOUT: device never parked")
    c.loop_stop(); c.disconnect()
    raise SystemExit(1)
t_park = time.time()
print("%s parked (sleep seen)" % stamp(), flush=True)
time.sleep(6)   # let it actually enter the park program

t_send = time.time()
c.publish(CMD, "capture", qos=1)
print("%s capture sent -> watching 60s" % stamp(), flush=True)
time.sleep(60)
c.loop_stop()
c.disconnect()

print("\n=== timeline (t=0 is the capture send) ===")
with lock:
    for t, kind, info in events:
        rel = t - t_send
        if t < t_park:
            continue
        print("%+8.2fs  %-5s  %s" % (rel, kind, info))

# verdict: anything between the capture send and the re-sleep 'sleep'
# (photos may land AFTER the sleep evt - the ~100KB photo packet is
# delivered slower than the small evt; allow a photo tail past sleep)
with lock:
    after = [e for e in events if e[0] >= t_send]
seq = [(e[1], e[2]) for e in after]
idx_sleep = next((i for i, (k, v) in enumerate(seq)
                  if k == "evt" and v == "sleep"), len(seq))
window = [v for (k, v) in seq[:idx_sleep] if k == "evt"]
bad = [v for v in window if v in ("awake", "hb")]
photo_ok = any(k == "photo" and v > 50000 for (k, v) in seq)
print("\nVERDICT: capture=%s photo=%s sleep=%s | awake/hb-in-window=%s"
      % ("capture" in window, photo_ok, idx_sleep < len(seq),
         bad if bad else "NONE"))
