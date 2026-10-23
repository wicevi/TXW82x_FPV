#!/usr/bin/env python3
"""http_recv.py - bench receiver for the NE102 wake-capture HTTP test.

Runs a plain HTTP server; every POST body is saved under ./wc_recv/ with
a timestamped name and logged (client, size). Responds 200 "ok" so the
firmware's wc_http client sees a clean status.

Usage:
    python http_recv.py [port]           # default 8080, binds 0.0.0.0

Firmware side (power test demo, CUSTOMER_ID=11):
    AT+PWCURL=http://<this-pc-ip>:8080/upload
    AT+PWCAP=2,10                        # HTTP-only cycle, 10s park
    AT+PWCAP=3,10                        # SD + HTTP
Then check wc_recv/*.jpg (should open as a JPEG from the camera).
"""

import os
import sys
import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RECV_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wc_recv")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        name = datetime.datetime.now().strftime("WC_%H%M%S_%f") + ".jpg"
        os.makedirs(RECV_DIR, exist_ok=True)
        path = os.path.join(RECV_DIR, name)
        with open(path, "wb") as f:
            f.write(body)
        soi = body[:2] == b"\xff\xd8"
        print("[%s] %s POST %s -> %s (%d bytes, JPEG SOI: %s)" % (
            datetime.datetime.now().strftime("%H:%M:%S"),
            self.client_address[0], self.path, name, len(body),
            "yes" if soi else "NO"))
        reply = b"ok"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def do_GET(self):
        reply = b"wc receiver alive\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, *args):  # silence the default per-request noise
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print("wc receiver on 0.0.0.0:%d, saving to %s" % (port, RECV_DIR))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
