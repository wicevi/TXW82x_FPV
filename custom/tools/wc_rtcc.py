#!/usr/bin/env python3
"""wc_rtcc.py - RTCC cold-boot capture leg, tolerant of the serial port
dropping when the RTCC park power-cycles the board (the CH340 USB link
glitches and Windows invalidates the handle - wc_test1.py died on it).

Flow: arm AT+PWCAP=1,8,,5 -> RTCC park (port may drop) -> REBOOT ->
cold-boot capture report -> re-sleep RTCC (port may drop again) -> idle
boot. Reconnects as needed; logs everything to wc_rtcc.log.
"""

import os
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "wc_rtcc.log")
PORT = "COM7"
BAUD = 921600
TOTAL_S = 150


def main():
    f = open(LOG, "ab", buffering=0)
    t0 = time.time()

    def log(text):
        line = "[%6.1f] %s" % (time.time() - t0, text)
        f.write((line + "\n").encode("utf-8", "replace"))
        print(line, flush=True)

    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    log("port open, arming RTCC cycle (mode=1 sd, 8s)")

    def send(c):
        try:
            ser.write((c + "\r\n").encode())
            log(">> " + c)
        except Exception as e:
            log("write fail: %s" % e)

    send("AT+PWCAP=1,8,,5")
    deadline = time.time() + TOTAL_S
    while time.time() < deadline:
        try:
            d = ser.read(4096)
            if d:
                text = d.decode("utf-8", "replace")
                for ln in text.splitlines():
                    if ln.strip():
                        log("<< " + ln[:180])
        except Exception as e:
            log("port lost (%s) - reconnecting..." % str(e)[:60])
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            while time.time() < deadline:
                try:
                    ser = serial.Serial(PORT, BAUD, timeout=0.2)
                    log("port back")
                    break
                except Exception:
                    time.sleep(1)
            if not ser:
                break
    log("window end")
    if ser:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
