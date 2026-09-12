#!/usr/bin/env python3
"""Minimal classic + CPE server: dark room, fire block placed by the server, then the block
definition of fire is changed to full bright (like /b edit fullbright on MCGalaxy)."""
import socket, struct, gzip, threading, time, sys

W, H, L = 32, 24, 32
blocks = bytearray(W * H * L)
def idx(x, y, z): return (y * L + z) * W + x
def fill(x1, y1, z1, x2, y2, z2, b):
    for y in range(y1, y2 + 1):
        for z in range(z1, z2 + 1):
            for x in range(x1, x2 + 1): blocks[idx(x, y, z)] = b
fill(0, 0, 0, W-1, 9, L-1, 1)
fill(6, 10, 6, 25, 18, 25, 4)
fill(8, 10, 8, 23, 16, 23, 0)

def s64(t): return t.encode().ljust(64)
def define_fire(fullbright):
    # DefineBlock (0x23): id, name, solidity, speed, top, side, bottom, transmitsLight, sound, fullBright, shape, draw, fog, r,g,b
    return bytes([0x23, 54]) + s64("Fire") + bytes([0, 128, 38, 38, 38, 1, 5, fullbright, 0, 0, 0, 0, 0, 0])
def envcol(var, r, g, b): return bytes([0x19, var]) + struct.pack('>hhh', r, g, b)

def serve(c):
    c.recv(131)  # identification
    c.sendall(bytes([0x10]) + s64("FakeServer") + struct.pack('>h', 3))
    for name in ("EnvColors", "BlockDefinitions", "CustomBlocks"):
        c.sendall(bytes([0x11]) + s64(name) + struct.pack('>i', 1))
    buf = b''
    while len(buf) < 67: buf += c.recv(4096)
    n = struct.unpack('>h', buf[65:67])[0]; buf = buf[67:]
    while len(buf) < 69 * n: buf += c.recv(4096)
    c.sendall(bytes([0x00, 7]) + s64("Fake") + s64("motd") + bytes([0x64]))
    c.sendall(bytes([0x13, 1]))
    c.sendall(define_fire(0))   # fire defined as NOT emitting first
    c.sendall(bytes([0x02]))
    data = gzip.compress(struct.pack('>i', len(blocks)) + bytes(blocks))
    for i in range(0, len(data), 1024):
        chunk = data[i:i+1024]
        c.sendall(bytes([0x03]) + struct.pack('>h', len(chunk)) + chunk.ljust(1024, b'\0') + bytes([min(100, (i + 1024) * 100 // len(data))]))
    c.sendall(bytes([0x04]) + struct.pack('>hhh', W, H, L))
    # env colours must come after the level: loading a level resets the environment
    c.sendall(envcol(0, 20, 20, 40) + envcol(3, 30, 30, 30) + envcol(4, 255, 255, 255))
    c.sendall(bytes([0x07, 0xFF]) + s64("tester") + struct.pack('>hhh', 9 * 32 + 16, 12 * 32, 9 * 32 + 16) + bytes([96, 10]))
    def drain():
        try:
            while c.recv(4096): pass
        except OSError: pass
    threading.Thread(target=drain, daemon=True).start()
    time.sleep(4)
    c.sendall(bytes([0x06]) + struct.pack('>hhh', 16, 10, 16) + bytes([54]))   # server places fire
    print("placed fire", flush=True)
    time.sleep(6)
    c.sendall(define_fire(1))   # now make it full bright
    print("redefined fire as fullbright", flush=True)
    while True:
        time.sleep(1); c.sendall(bytes([0x01]))

srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', 25565)); srv.listen(2)
print("listening", flush=True)
while True:
    c, _ = srv.accept()
    try: serve(c)
    except OSError as e: print("client gone", e, flush=True)
