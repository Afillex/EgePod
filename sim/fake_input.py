#!/usr/bin/env python3
"""Inject fake touch/key events into /dev/uinput for EgePod simulation.

Usage:
  python3 sim/fake_input.py tap <x> <y>
  python3 sim/fake_input.py swipe_left
  python3 sim/fake_input.py swipe_right
  python3 sim/fake_input.py power
  python3 sim/fake_input.py volume_up
  python3 sim/fake_input.py volume_down
"""

import sys
import os
import struct
import time
import fcntl

# linux/uinput.h constants
UI_SET_EVBIT   = 0x40045564
UI_SET_KEYBIT  = 0x40045565
UI_SET_ABSBIT  = 0x40045567
UI_DEV_CREATE  = 0x5501
UI_DEV_DESTROY = 0x5502

EV_SYN  = 0x00
EV_KEY  = 0x01
EV_ABS  = 0x03

SYN_REPORT = 0
ABS_MT_TRACKING_ID  = 0x39
ABS_MT_POSITION_X   = 0x35
ABS_MT_POSITION_Y   = 0x36
ABS_MT_SLOT         = 0x2F

KEY_POWER      = 116
KEY_VOLUMEUP   = 115
KEY_VOLUMEDOWN = 114
BTN_TOUCH      = 0x14a

UINPUT_DEV_FMT = "80sHHI" + "i" * 64 * 4  # uinput_user_dev

def emit(fd, ev_type, ev_code, ev_value):
    t = time.time()
    sec  = int(t)
    usec = int((t - sec) * 1e6)
    event = struct.pack("llHHi", sec, usec, ev_type, ev_code, ev_value)
    os.write(fd, event)

def sync(fd):
    emit(fd, EV_SYN, SYN_REPORT, 0)

def create_touch_device():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)

    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
    fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH)
    fcntl.ioctl(fd, UI_SET_KEYBIT, KEY_POWER)
    fcntl.ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEUP)
    fcntl.ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN)
    fcntl.ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID)
    fcntl.ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X)
    fcntl.ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y)
    fcntl.ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT)

    # uinput_user_dev: name(80) id(4 shorts) ff_effects_max(uint) absmax/min/fuzz/flat(256 ints)
    name = b"egepod-sim-touch"
    udev = struct.pack(
        "=80sHHHHI" + "i"*256,
        name.ljust(80, b'\x00'),
        0x0001, 0x0001, 0x0001, 0x0001,  # bustype, vendor, product, version
        0,                                # ff_effects_max
        *([1080, 2400, 0, 0, 0, 0, 0, 0] + [0]*248)  # abs_max, abs_min... (simplified)
    )
    os.write(fd, udev)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(0.2)  # let the kernel register the device
    return fd

def inject_tap(fd, x, y):
    emit(fd, EV_ABS, ABS_MT_SLOT, 0)
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 1)
    emit(fd, EV_ABS, ABS_MT_POSITION_X, x)
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, y)
    emit(fd, EV_KEY, BTN_TOUCH, 1)
    sync(fd)
    time.sleep(0.05)
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1)
    emit(fd, EV_KEY, BTN_TOUCH, 0)
    sync(fd)
    print(f"Injected tap at ({x}, {y})")

def inject_swipe(fd, x0, y0, x1, y1, steps=10):
    emit(fd, EV_ABS, ABS_MT_SLOT, 0)
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 1)
    emit(fd, EV_KEY, BTN_TOUCH, 1)
    for i in range(steps + 1):
        x = x0 + (x1 - x0) * i // steps
        y = y0 + (y1 - y0) * i // steps
        emit(fd, EV_ABS, ABS_MT_POSITION_X, x)
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, y)
        sync(fd)
        time.sleep(0.01)
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1)
    emit(fd, EV_KEY, BTN_TOUCH, 0)
    sync(fd)
    print(f"Injected swipe ({x0},{y0}) → ({x1},{y1})")

def inject_key(fd, keycode):
    emit(fd, EV_KEY, keycode, 1)
    sync(fd)
    time.sleep(0.05)
    emit(fd, EV_KEY, keycode, 0)
    sync(fd)

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]
    fd  = create_touch_device()

    try:
        if cmd == "tap":
            x = int(sys.argv[2]) if len(sys.argv) > 2 else 540
            y = int(sys.argv[3]) if len(sys.argv) > 3 else 950
            inject_tap(fd, x, y)
        elif cmd == "swipe_left":
            inject_swipe(fd, 900, 1200, 180, 1200)
        elif cmd == "swipe_right":
            inject_swipe(fd, 180, 1200, 900, 1200)
        elif cmd == "power":
            inject_key(fd, KEY_POWER)
            print("Injected KEY_POWER")
        elif cmd == "volume_up":
            inject_key(fd, KEY_VOLUMEUP)
            print("Injected KEY_VOLUMEUP")
        elif cmd == "volume_down":
            inject_key(fd, KEY_VOLUMEDOWN)
            print("Injected KEY_VOLUMEDOWN")
        else:
            print(f"Unknown command: {cmd}")
            print(__doc__)
            sys.exit(1)
    finally:
        time.sleep(0.1)
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)

if __name__ == "__main__":
    main()
