#!/usr/bin/env python3
"""wc_test1.py - unattended board validation for the synchronous-sleep
wrapper + wake-capture cycle (runs as soon as COM7 frees up).

Sequence:
  1. wait for COM7 (poll, up to ~35 min - the user's terminal may hold it)
  2. AT+PWR?            liveness + IP
  3. AT+PWRS=1,10       park regression through pt_sleep_sync
  4. AT+PWRDT=10        DTIM-timed regression (needs association)
  5. AT+PWCAP?          status
  6. AT+PWCAP=1,10      SD cycle
  7. http_recv.py on this PC + AT+PWCURL + AT+PWCAP=2,10   HTTP cycle
  8. AT+PWCAP=3,10      both
Writes everything to wc_test1.log next to this script.
"""

import os
import socket
import subprocess
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "wc_test1.log")
PORT = "COM7"
BAUD = 921600


def pc_lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return None
    finally:
        s.close()


class Console:
    def __init__(self):
        self.ser = None
        self.f = open(LOG, "ab", buffering=0)
        self.t0 = time.time()

    def log(self, text):
        stamp = "[%7.1f] " % (time.time() - self.t0)
        line = (stamp + text).encode("utf-8", "replace")
        self.f.write(line + b"\n")
        sys.stdout.write(line.decode() + "\n")
        sys.stdout.flush()

    def drain(self, quiet=0.5):
        """Read until quiet. The RTCC park power-cycles the board and the
        CH340 USB-serial glitches (Windows invalidates the handle) - on
        that exception, reconnect instead of dying (wc_test1 died on the
        RTCC leg before this fix)."""
        buf = b""
        t0 = time.time()
        while time.time() - t0 < quiet:
            try:
                d = self.ser.read(4096)
            except Exception as e:
                self.log("port lost (%s) - reconnecting..." % str(e)[:50])
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
                while self.ser is None:
                    try:
                        self.ser = serial.Serial(PORT, BAUD, timeout=0.2)
                        self.log("port back")
                    except Exception:
                        time.sleep(1)
                t0 = time.time()
                continue
            if d:
                buf += d
                t0 = time.time()
        if buf:
            self.log("<< " + buf.decode("utf-8", "replace").rstrip())
        return buf.decode("utf-8", "replace")

    def cmd(self, c, wait):
        self.ser.reset_input_buffer()
        self.ser.write((c + "\r\n").encode())
        self.log(">> " + c)
        time.sleep(0.3)
        return self.drain(wait)

    def wait_port(self, timeout_s):
        deadline = time.time() + timeout_s
        n = 0
        while time.time() < deadline:
            try:
                self.ser = serial.Serial(PORT, BAUD, timeout=0.2)
                self.log("port %s acquired" % PORT)
                return True
            except Exception as e:
                n += 1
                if n % 6 == 1:
                    self.log("waiting for %s (%s) ..." % (PORT, e))
                time.sleep(10)
        return False


def main():
    c = Console()
    c.log("=== wc_test1 start (sync-sleep wrapper + PWCAP validation) ===")
    if not c.wait_port(8 * 3600):
        c.log("FATAL: COM7 never freed - giving up")
        return 2
    try:
        time.sleep(0.5)
        c.drain(1.5)
        c.cmd("AT+PWR?", 4)
        c.cmd("AT+PWRS=1,10", 40)
        c.cmd("AT+PWRDT=10", 45)
        c.cmd("AT+PWCAP?", 4)
        c.cmd("AT+PWCAP=1,10", 75)

        ip = pc_lan_ip()
        if ip:
            recv = subprocess.Popen(
                [sys.executable, os.path.join(HERE, "http_recv.py"), "8080"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.5)
            c.cmd("AT+PWCURL=http://%s:8080/upload" % ip, 4)
            c.cmd("AT+PWCAP=2,10", 75)
            c.cmd("AT+PWCAP=3,10", 75)
            recv.terminate()
        else:
            c.log("no LAN IP on this PC - skipping HTTP legs")
        # RTCC reboot path: arm -> RTCC park -> REBOOT -> cold-boot
        # capture (SD) -> re-sleep RTCC -> idle boot. Long window: the
        # serial stays open across the chip reboots.
        c.cmd("AT+PWCAP=1,8,,5", 130)
        c.cmd("AT+PWCAP?", 4)
        c.log("=== done ===")
        return 0
    finally:
        if c.ser:
            c.ser.close()
        c.log("port released")


if __name__ == "__main__":
    sys.exit(main())
