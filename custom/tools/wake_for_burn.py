"""One-shot: publish 'wake' to the device cmd topic and wait for the
wake event (device then stays awake - safe window for a CKLink burn)."""
import json
import threading
import time

import paho.mqtt.client as mqtt

MAC = "433ba8"
CMD = "ne102/%s/cmd" % MAC
EVT = "ne102/%s/evt" % MAC

got = threading.Event()
seen = []

def on_evt(c, u, m):
    try:
        d = json.loads(m.payload)
    except Exception:
        return
    seen.append((time.time(), d.get("event")))
    if d.get("event") in ("wake", "awake"):
        got.set()

c = mqtt.Client(client_id="ne102_wake_probe")
c.on_message = on_evt
c.connect("broker.emqx.io", 1883, 60)
c.subscribe(EVT, qos=1)
c.loop_start()

c.publish(CMD, "wake", qos=1)
print("sent wake, waiting for wake/awake event ...", flush=True)
ok = got.wait(45)
time.sleep(2)
c.loop_stop()
c.disconnect()
print("RESULT:", "WOKEN" if ok else "TIMEOUT", seen[-6:])
