#!/usr/bin/env python3
"""NE102 MQTT keepalive-sleep test console (firmware: custom/component/mqttka).

Subscribes the device event topic, prints every event (online/sleep/hb/
wake/awake) and forwards your typed lines to the command topic:
  sleep  -> device enters / keeps WiFi keepalive sleep
  wake   -> device exits the sleep loop and publishes a wake event
  other  -> device prints the payload on its serial console only

Usage:
  python mqttka_console.py [mac6]              # e.g. python mqttka_console.py 0af1e2
  MQTT_HOST=broker.emqx.io MQTT_PORT=1883 python mqttka_console.py

Defaults: broker.emqx.io:1883, topics ne102/<mac6>/cmd + ne102/<mac6>/evt.
Without mac6 it subscribes ne102/+/evt and publishes to the mac6 you type
as "<mac6> <text>" (so multiple boards can share one console).
"""
import os
import sys
import threading
import time

import paho.mqtt.client as mqtt

HOST = os.environ.get("MQTT_HOST", "broker.emqx.io")
PORT = int(os.environ.get("MQTT_PORT", "1883"))
MAC6 = sys.argv[1] if len(sys.argv) > 1 else None

SUB = ("ne102/%s/evt" % MAC6) if MAC6 else "ne102/+/evt"
PUB = ("ne102/%s/cmd" % MAC6) if MAC6 else None


def on_connect(client, userdata, flags, rc):
    print("[console] connected to %s:%d (rc=%d), sub %s" % (HOST, PORT, rc, SUB))
    client.subscribe(SUB)


def on_message(client, userdata, msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg.payload.decode(errors="replace")))


def main():
    c = mqtt.Client(client_id="ne102_console_%d" % int(time.time()))
    c.on_connect = on_connect
    c.on_message = on_message
    c.connect(HOST, PORT, keepalive=60)
    c.loop_start()
    print("[console] type commands (sleep/wake/text%s), Ctrl-C to quit"
          % ("" if MAC6 else ", prefix with '<mac6> '"))
    try:
        while True:
            try:
                line = input().strip()
            except EOFError:
                # background/headless mode (stdin closed): keep observing
                time.sleep(0.5)
                continue
            if not line:
                continue
            if PUB:
                topic, text = PUB, line
            else:
                parts = line.split(None, 1)
                if len(parts) < 2:
                    print("[console] format: <mac6> <text>")
                    continue
                topic, text = "ne102/%s/cmd" % parts[0], parts[1]
            c.publish(topic, text, qos=0)
            print("[console] -> %s: %s" % (topic, text))
    except KeyboardInterrupt:
        pass
    finally:
        c.loop_stop()
        c.disconnect()


if __name__ == "__main__":
    main()
