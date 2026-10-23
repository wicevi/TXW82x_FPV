#!/usr/bin/env python3
"""NE102 MQTT photo receiver (NE301-compatible JSON+Base64 protocol).
Usage: MQTT_HOST=127.0.0.1 MQTT_PORT=9991 python mqtt_recv_test.py [sec] [topic]"""
import base64, json, os, sys, time
import paho.mqtt.client as mqtt

DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 60
TOPIC = sys.argv[2] if len(sys.argv) > 2 else "ne102/+/photo"
OUT = os.environ.get("NE102_OUT", "ne102_photos")
os.makedirs(OUT, exist_ok=True)

images = {}

def on_msg(client, userdata, msg):
    try:
        d = json.loads(msg.payload)
    except Exception:
        print("SKIP: not JSON (%d bytes)" % len(msg.payload))
        return
    t = d.get("type", "")
    if t == "image_chunked_header":
        iid = d.get("image_id", "?")
        images[iid] = {"chunks": {}, "total": d.get("total_chunks", 0),
                       "meta": d.get("metadata", {})}
        print("HEADER: id=%s chunks=%d size=%d" % (iid, images[iid]["total"], d.get("total_size", 0)))
    elif t == "image_chunk":
        iid = d.get("image_id", "?")
        if iid not in images:
            images[iid] = {"chunks": {}, "total": d.get("total_chunks", 0), "meta": {}}
        e = images[iid]
        try:
            e["chunks"][d.get("chunk_index", -1)] = base64.b64decode(d.get("data", ""))
        except Exception as ex:
            print("B64 ERR: %s" % ex)
            return
        if len(e["chunks"]) == e["total"]:
            blob = b"".join(e["chunks"][i] for i in range(e["total"]))
            save(blob, "chunked trigger=%s" % e["meta"].get("trigger_type", "?"))
    else:
        img = d.get("image_data", d.get("image", ""))
        if img.startswith("data:image/jpeg;base64,"):
            try:
                blob = base64.b64decode(img[22:])
            except Exception as ex:
                print("B64 ERR: %s" % ex)
                return
            save(blob, "single trigger=%s" % d.get("metadata", {}).get("trigger_type", "?"))

def save(blob, tag):
    fn = os.path.join(OUT, "img_%d.jpg" % int(time.time() * 1000))
    with open(fn, "wb") as f:
        f.write(blob)
    ok = blob[:2] == b"\xff\xd8" and blob[-2:] == b"\xff\xd9"
    print("IMAGE SAVED: %s %dB SOI/EOI=%s (%s)" % (fn, len(blob), ok, tag))

c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
c.on_message = on_msg
c.connect(os.environ.get("MQTT_HOST", "127.0.0.1"), int(os.environ.get("MQTT_PORT", "9991")), 60)
c.subscribe(TOPIC, qos=1)
c.loop_start()
print("subscribed %s on %s:%s for %ds" % (TOPIC, os.environ.get("MQTT_HOST", "127.0.0.1"), os.environ.get("MQTT_PORT", "9991"), DUR))
time.sleep(DUR)
c.loop_stop()
