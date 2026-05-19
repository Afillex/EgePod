#!/usr/bin/env python3
"""Send only CMD_PLAY — let playback_thread load track 0 itself."""
import socket, struct, time

AUDIOD_SOCK = "/tmp/egepod_audiod.sock"

sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
sock.connect(AUDIOD_SOCK)

data = sock.recv(512)
msg_size = len(data)
print(f"IpcMsg size = {msg_size}, index_ready type=0x{data[0]:02x}")

# Send CMD_PLAY only
play_msg = bytearray(msg_size)
play_msg[0] = 0x01  # CMD_PLAY
sock.send(bytes(play_msg))
print("CMD_PLAY sent")

# Listen for events
print("Waiting for events (15s)...")
sock.settimeout(15.0)
start = time.time()
try:
    while time.time() - start < 14:
        data = sock.recv(512)
        t = data[0]
        v = struct.unpack_from('<I', data, 8)[0]
        names = {0x81:'STATE', 0x82:'TRACK', 0x83:'POSITION', 0x84:'INDEX_READY', 0x85:'ERROR'}
        elapsed = time.time() - start
        print(f"  t={elapsed:.1f}s  EVT 0x{t:02x} ({names.get(t,'?')}): {v}")
except Exception as e:
    print(f"end: {e}")

sock.close()
