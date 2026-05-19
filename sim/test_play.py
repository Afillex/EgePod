#!/usr/bin/env python3
"""Send LOAD_TRACK(0) + PLAY to audiod, listen for events.

IpcMsg actual size on ARM64 = 1+3+4 + sizeof(TrackInfo)
TrackInfo = 512+256+256+256+4+4+4+4+4 = 1300, so IpcMsg = 1308 bytes.
We probe it from a large recv buffer first.
"""
import socket, struct, time

AUDIOD_SOCK = "/tmp/egepod_audiod.sock"

sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
sock.connect(AUDIOD_SOCK)

# Receive idx_ready with a large buffer to get true IpcMsg size
data = sock.recv(65536)
msg_size = len(data)
print(f"IpcMsg size = {msg_size}, type=0x{data[0]:02x}")

def send_cmd(cmd_type, param_u32=0):
    msg = bytearray(msg_size)
    msg[0] = cmd_type
    struct.pack_into('<I', msg, 8, param_u32)
    n = sock.send(bytes(msg))
    print(f"  sent {n} bytes (cmd=0x{cmd_type:02x})")
    try:
        sock.settimeout(2.0)
        r = sock.recv(65536)
        print(f"  reply: size={len(r)} type=0x{r[0]:02x}")
        return r
    except Exception as e:
        print(f"  no reply: {e}")
        return None

print("Loading track 0...")
send_cmd(0x07, 0)   # CMD_LOAD_TRACK, idx=0

print("Sending PLAY...")
send_cmd(0x01)      # CMD_PLAY

print("Listening for position events (20s)...")
sock.settimeout(20.0)
start = time.time()
try:
    while time.time() - start < 19:
        data = sock.recv(65536)
        t = data[0]
        v = struct.unpack_from('<I', data, 8)[0]
        names = {0x81:'STATE', 0x82:'TRACK', 0x83:'POSITION', 0x84:'INDEX', 0x85:'ERROR'}
        elapsed = time.time() - start
        print(f"  t={elapsed:.1f}s  EVT 0x{t:02x} ({names.get(t,'?')}): {v}")
except Exception as e:
    print(f"end: {e}")

sock.close()
