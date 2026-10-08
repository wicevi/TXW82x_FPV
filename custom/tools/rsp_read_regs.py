"""GDB-remote client with proper handshake for CSKY DebugServer.

Mimics what a real GDB does: qSupported first, then halt query, register read.
"""
import socket

HOST, PORT = "127.0.0.1", 1025

def cksum(d: bytes) -> int:
    return sum(d) & 0xFF

class Rsp:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=10)
        self.buf = b""

    def send_raw(self, b: bytes):
        self.s.sendall(b)

    def send(self, cmd: str):
        p = cmd.encode()
        self.send_raw(b"$" + p + b"#" + f"{cksum(p):02x}".encode())

    def recv_pkt(self, timeout=10) -> bytes:
        self.s.settimeout(timeout)
        while True:
            if b"#" in self.buf and len(self.buf.split(b"#", 1)[1]) >= 2:
                break
            c = self.s.recv(4096)
            if not c:
                raise ConnectionError("closed by server")
            self.buf += c
        body, rest = self.buf.split(b"#", 1)
        self.buf = rest[2:]
        self.send_raw(b"+")
        return body[1:]

    def cmd(self, c: str, timeout=10) -> bytes:
        self.send(c)
        while True:
            pkt = self.recv_pkt(timeout)
            if not pkt.startswith(b"o"):  # skip console output packets
                return pkt

def main():
    r = Rsp(HOST, PORT)
    # GDB-style handshake
    print("qSupported ->", r.cmd("qSupported:multiprocess+;swbreak+;hwbreak+;qRelocInsn+;fork-events+;vfork-events+;exec-events+;vContSupported+;QThreadEvents+;no-resumed+;xmlRegisters=i386").decode(errors="replace"))
    for q in ["QStartNoAckMode+", "qTStatus", "?"]:
        try:
            print(q, "->", r.cmd(q).decode(errors="replace")[:120])
        except Exception as e:
            print(q, "ERR", e)
            break
    g = r.cmd("g").decode(errors="replace")
    print("g len:", len(g))
    data = bytes.fromhex(g)
    w = [int.from_bytes(data[i*4:i*4+4], "little") for i in range(len(data)//4)]
    print("PC  = 0x%08x" % w[35])
    print("PSR = 0x%08x" % w[65] if len(w) > 65 else "?")
    print("SP  = 0x%08x" % w[14])
    for i in range(0, 16):
        print("r%-2d = 0x%08x" % (i, w[i]))
    pc = w[35]
    m = r.cmd("m%x,32" % (pc & ~3)).decode(errors="replace")
    print("mem@[pc]:", m)
    # detach politely
    try:
        r.cmd("D")
    except Exception:
        pass

if __name__ == "__main__":
    main()
