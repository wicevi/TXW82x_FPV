"""Serial console logger for the NE102 device console.
Usage: python serial_log.py [COM7] [921600]
Writes console_log.txt with elapsed-time stamps.
"""
import serial
import time
import os
import sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 921600
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "console_log.txt")
STOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "serial_stop")

ser = serial.Serial(PORT, BAUD, timeout=0.2, bytesize=8, parity="N", stopbits=1)
f = open(OUT, "ab", buffering=0)
print("logging %s @%d -> %s" % (PORT, BAUD, OUT), flush=True)
t0 = time.time()
try:
    while not os.path.exists(STOP):
        data = ser.read(4096)
        if data:
            stamp = ("[%8.2f] " % (time.time() - t0)).encode()
            f.write(stamp)
            f.write(data)
            f.write(b"\n")
except Exception as e:
    print("logger exit:", e, flush=True)
finally:
    ser.close()
    f.close()
