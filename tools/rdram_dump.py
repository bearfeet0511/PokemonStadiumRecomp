"""Dump the running game's 8 MB of RDRAM through the debug server (rdram_peek).

    python tools/rdram_dump.py out.bin [--port 4371]

The file is in N64 byte order (big-endian), so offset X is guest address
0x80000000 + X. Takes a few seconds; the game keeps running meanwhile, so
dump while the screen of interest is paused or idle.
"""
import argparse
import json
import socket

RDRAM_SIZE = 8 * 1024 * 1024
CHUNK = 256


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("--port", type=int, default=4371)
    args = parser.parse_args()

    sock = socket.create_connection(("127.0.0.1", args.port))
    stream = sock.makefile("rwb")
    data = bytearray()
    for addr in range(0, RDRAM_SIZE, CHUNK):
        request = {"cmd": "rdram_peek", "addr": 0x80000000 + addr, "n": CHUNK}
        stream.write((json.dumps(request) + "\n").encode())
        stream.flush()
        reply = json.loads(stream.readline())
        if not reply.get("ok"):
            raise SystemExit(f"rdram_peek failed at {addr:#x}: {reply}")
        data += bytes.fromhex(reply["hex"])
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} bytes to {args.out}")


if __name__ == "__main__":
    main()
