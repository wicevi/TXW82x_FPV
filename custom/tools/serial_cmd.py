"""NE102 serial command sender + logger (COM7 default).

Usage:
  python serial_cmd.py                       # log only (same as serial_log)
  python serial_cmd.py "AT+NEMODE=1"         # send one command, log 3s
  python serial_cmd.py "AT+NECAP?" -w 8      # custom wait window
  python serial_cmd.py -b                    # log until serial_stop appears

Writes cmd_log.txt with elapsed-time stamps; prints to stdout.
Exit code 0. Requires pyserial.
"""
import serial
import time
import os
import sys

PORT = "COM7"
BAUD = 921600
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cmd_log.txt")
STOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "serial_stop")

args = [a for a in sys.argv[1:]]
wait = 3.0
cmds = []
i = 0
while i < len(args):
    if args[i] == "-w":
        wait = float(args[i + 1]); i += 2; continue
    if args[i] == "-b":
        wait = None; i += 1; continue
    cmds.append(args[i]); i += 1

if os.path.exists(STOP):
    os.remove(STOP)

ser = serial.Serial(PORT, BAUD, timeout=0.2, bytesize=8, parity="N", stopbits=1)
f = open(OUT, "ab", buffering=0)
t0 = time.time()

def log(data):
    stamp = ("[%8.2f] " % (time.time() - t0)).encode()
    f.write(stamp); f.write(data); f.write(b"\n")
    sys.stdout.write(data.decode("utf-8", "replace")); sys.stdout.flush()

print("cmd session %s @%d -> %s" % (PORT, BAUD, OUT), flush=True)
try:
    # drain pending output first
    time.sleep(0.3)
    while True:
        data = ser.read(4096)
        if not data:
            break
        log(data)

    for c in cmds:
        ser.write((c + "\r\n").encode())
        log((">>> %s\n" % c).encode())

    if wait is None:
        while not os.path.exists(STOP):
            data = ser.read(4096)
            if data:
                log(data)
    else:
        end = time.time() + wait
        while time.time() < end:
            data = ser.read(4096)
            if data:
                log(data)
except Exception as e:
    print("cmd session exit:", e, flush=True)
finally:
    ser.close()
    f.close()
