#!/usr/bin/env python3
"""Minimal CLI client for the s2sc device agent.

Speaks the wire protocol from shared/proto.h directly, so it can drive the agent
without the ImGui app (handy when the agent is already running on the device).

Usage:
    python agent_probe.py hello
    python agent_probe.py procs [filter]
    python agent_probe.py regions <pid> [filter]
    python agent_probe.py read <pid> <addr-hex> <len>
"""

import socket
import struct
import sys

MAGIC = 0x43533253

OP_HELLO = 1
OP_LISTPROC = 2
OP_ATTACH = 3
OP_REGIONS = 4
OP_READ = 5
OP_WRITE = 6
OP_CHAIN = 7
OP_PING = 8
OP_DETACH = 9


def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("agent closed the connection")
        buf += chunk
    return buf


def call(sock, op, payload=b""):
    sock.sendall(struct.pack("<IHHI", MAGIC, op, 0, len(payload)) + payload)
    magic, rop, _flags, length = struct.unpack("<IHHI", _recv_exact(sock, 12))
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}")
    body = _recv_exact(sock, length) if length else b""
    return rop, body


def connect(host="127.0.0.1", port=31337):
    s = socket.create_connection((host, port), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def hello(sock):
    _, body = call(sock, OP_HELLO)
    proto, pid_max, page, build = struct.unpack("<III64s", body[:76])
    return proto, pid_max, page, build.split(b"\0")[0].decode(errors="replace")


def procs(sock):
    _, body = call(sock, OP_LISTPROC)
    out = []
    for off in range(0, len(body) - 67, 68):
        pid, name = struct.unpack_from("<I64s", body, off)
        out.append((pid, name.split(b"\0")[0].decode(errors="replace")))
    return out


def attach(sock, pid):
    _, body = call(sock, OP_ATTACH, struct.pack("<I", pid))
    return struct.unpack("<i", body[:4])[0]


def regions(sock):
    _, body = call(sock, OP_REGIONS)
    out = []
    # RegionEntry: u64 start, u64 end, u32 prot, u32 offset, char name[128]
    stride = struct.calcsize("<QQII128s")
    for off in range(0, len(body) - stride + 1, stride):
        start, end, prot, foff, name = struct.unpack_from("<QQII128s", body, off)
        out.append((start, end, prot, foff, name.split(b"\0")[0].decode(errors="replace")))
    return out


def read(sock, addr, length):
    _, body = call(sock, OP_READ, struct.pack("<QI", addr, length))
    status = struct.unpack("<i", body[:4])[0]
    if status != 0:
        raise OSError(f"agent read failed: status={status}")
    return body[4:]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    cmd = argv[1]
    sock = connect()
    try:
        if cmd == "hello":
            proto, pid_max, page, build = hello(sock)
            print(f"proto={proto} pid_max={pid_max} page={page} build={build!r}")
        elif cmd == "procs":
            needle = argv[2].lower() if len(argv) > 2 else ""
            for pid, name in procs(sock):
                if needle and needle not in name.lower():
                    continue
                print(f"{pid:>7}  {name}")
        elif cmd == "regions":
            pid = int(argv[2])
            st = attach(sock, pid)
            if st != 0:
                print(f"attach failed: status={st}")
                return 1
            needle = argv[3].lower() if len(argv) > 3 else ""
            total = 0
            for start, end, prot, foff, name in regions(sock):
                total += end - start
                if needle and needle not in name.lower():
                    continue
                p = ("r" if prot & 1 else "-") + ("w" if prot & 2 else "-") + ("x" if prot & 4 else "-")
                print(f"{start:012x}-{end:012x} {p} off={foff:08x} {name}")
            print(f"# total mapped: {total / (1 << 20):.1f} MiB")
        elif cmd == "read":
            pid = int(argv[2])
            addr = int(argv[3], 16)
            length = int(argv[4], 0)
            st = attach(sock, pid)
            if st != 0:
                print(f"attach failed: status={st}")
                return 1
            data = read(sock, addr, length)
            for i in range(0, len(data), 16):
                chunk = data[i:i + 16]
                hexpart = " ".join(f"{b:02x}" for b in chunk)
                txt = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
                print(f"{addr + i:012x}  {hexpart:<47}  {txt}")
        else:
            print(__doc__)
            return 2
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
