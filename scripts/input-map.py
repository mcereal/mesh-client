#!/usr/bin/env python3
"""Decode what a TrimUI Brick's buttons report, from a capture taken on the device.

Two questions, two modes, because a press can only answer one of them:

  caps    reads /proc/bus/input/devices on stdin and prints what each device *can* emit.
          An absent code and an unpressed button look identical in a capture, so the
          capability bitmaps are the only way to know the difference - and they lie in the
          other direction: the Brick's pad declares KEY_F1/KEY_F2 and volume keys it never
          sends, so a mapping taken from the bitmap alone would be wrong too.

  presses reads a directory of raw event*.bin captures and groups them into presses, so a
          button's name is read off the case and its code off the wire.

Stdlib only, like scripts/frames.py: this runs on the development host, which is usually macOS,
and the capture is just bytes.

Usage:
    scripts/input-map.py caps < devices.txt
    scripts/input-map.py presses capture-dir/ [--gap SECONDS]
"""

import argparse
import glob
import os
import struct
import sys

# The subset of linux/input-event-codes.h these four devices can reach. Kept here rather than
# parsed from a header because the header is Linux-only and this script is not.
EV = {0: "EV_SYN", 1: "EV_KEY", 2: "EV_REL", 3: "EV_ABS", 5: "EV_SW", 17: "EV_LED", 20: "EV_REP"}

KEY = {
    1: "KEY_ESC",
    14: "KEY_BACKSPACE",
    15: "KEY_TAB",
    28: "KEY_ENTER",
    57: "KEY_SPACE",
    59: "KEY_F1",
    60: "KEY_F2",
    61: "KEY_F3",
    62: "KEY_F4",
    103: "KEY_UP",
    104: "KEY_PAGEUP",
    105: "KEY_LEFT",
    106: "KEY_RIGHT",
    108: "KEY_DOWN",
    109: "KEY_PAGEDOWN",
    113: "KEY_MUTE",
    114: "KEY_VOLUMEDOWN",
    115: "KEY_VOLUMEUP",
    116: "KEY_POWER",
    139: "KEY_MENU",
    142: "KEY_SLEEP",
    143: "KEY_WAKEUP",
    226: "KEY_MEDIA",
    # The gamepad space. The Brick's pad impersonates an Xbox 360 controller, so the face
    # buttons arrive under their compass names and nothing here reports by position: what is
    # printed A on the case is BTN_EAST. See docs/device.md.
    304: "BTN_SOUTH",
    305: "BTN_EAST",
    306: "BTN_C",
    307: "BTN_NORTH",
    308: "BTN_WEST",
    309: "BTN_Z",
    310: "BTN_TL",
    311: "BTN_TR",
    312: "BTN_TL2",
    313: "BTN_TR2",
    314: "BTN_SELECT",
    315: "BTN_START",
    316: "BTN_MODE",
    317: "BTN_THUMBL",
    318: "BTN_THUMBR",
    544: "BTN_DPAD_UP",
    545: "BTN_DPAD_DOWN",
    546: "BTN_DPAD_LEFT",
    547: "BTN_DPAD_RIGHT",
    582: "KEY_VOICECOMMAND",
}

ABS = {
    0: "ABS_X",
    1: "ABS_Y",
    2: "ABS_Z",
    3: "ABS_RX",
    4: "ABS_RY",
    5: "ABS_RZ",
    16: "ABS_HAT0X",
    17: "ABS_HAT0Y",
}

SW = {
    0: "SW_LID",
    1: "SW_TABLET_MODE",
    2: "SW_HEADPHONE_INSERT",
    4: "SW_MICROPHONE_INSERT",
    6: "SW_LINEOUT_INSERT",
}

TABLES = {"KEY": KEY, "ABS": ABS, "SW": SW}

# struct input_event on this device's aarch64 kernel: two 8-byte timeval fields, then u16 type,
# u16 code, s32 value. 24 bytes, and the capture is a whole number of them.
RECORD = struct.Struct("<qqHHi")


def code_name(etype, code):
    table = {1: KEY, 3: ABS, 5: SW}.get(etype)
    if table is None:
        return f"{EV.get(etype, etype)}:{code}"
    return table.get(code, f"unknown {code}")


def decode_bitmap(field):
    """Bit indices set in a /proc/bus/input/devices bitmap.

    The kernel prints one 64-bit word per field, highest word first, so the words have to be
    reversed before a bit index means a code number."""
    words = [int(word, 16) for word in field.split()]
    words.reverse()
    return [
        index * 64 + bit for index, word in enumerate(words) for bit in range(64) if word >> bit & 1
    ]


def cmd_caps(stream):
    """What each device can emit, from its capability bitmaps."""
    for line in stream:
        line = line.rstrip("\n")
        if line.startswith("N: Name="):
            print(f"\n{line.split('=', 1)[1].strip(chr(34))}")
        elif line.startswith("H: Handlers="):
            print(f"  handlers: {line.split('=', 1)[1].strip()}")
        elif line.startswith("B: ") and "=" in line:
            kind, field = line[3:].split("=", 1)
            table = TABLES.get(kind)
            if table is None:
                continue
            codes = decode_bitmap(field)
            if not codes:
                continue
            named = ", ".join(f"{code} {table.get(code, '?')}" for code in codes)
            print(f"  {kind}: {named}")
    return 0


def read_capture(directory):
    events = []
    for path in sorted(glob.glob(os.path.join(directory, "event*.bin"))):
        device = os.path.basename(path)[: -len(".bin")]
        with open(path, "rb") as handle:
            blob = handle.read()
        for offset in range(0, len(blob) - RECORD.size + 1, RECORD.size):
            seconds, micros, etype, code, value = RECORD.unpack_from(blob, offset)
            if etype == 0:  # EV_SYN carries no information for a button map.
                continue
            events.append((seconds + micros / 1e6, device, etype, code, value))
    events.sort()
    return events


def cmd_presses(directory, gap):
    events = read_capture(directory)
    if not events:
        print(f"no events in {directory}", file=sys.stderr)
        return 1

    # A gap ends a press only when nothing is still down. evdev is silent for the whole of a
    # hold - one event on the way down, one on the way up - so splitting on quiet alone tears a
    # two-second trigger hold into a [255] and a [0] two seconds apart, which is exactly the
    # measurement this was written to take.
    #
    # A switch is the exception and has to be, because it reports a state rather than a press:
    # SW_TABLET_MODE goes to 1 and stays there, so counting it as held would swallow every
    # press after it into one group.
    groups, current, previous, held = [], [], None, set()
    for event in events:
        timestamp, device, etype, code, value = event
        if previous is not None and timestamp - previous > gap and not held:
            groups.append(current)
            current = []
        current.append(event)
        previous = timestamp
        if etype != 5:  # EV_SW
            if value == 0:
                held.discard((device, etype, code))
            else:
                held.add((device, etype, code))
    groups.append(current)

    print(f"{len(events)} events in {len(groups)} presses\n")
    for number, group in enumerate(groups, 1):
        # One line per code, in the order the codes first appeared, carrying every value it
        # took - so a press/release pair reads as [1, 0] and a trigger as [255, 0].
        order, values = [], {}
        for _, device, etype, code, value in group:
            key = (device, etype, code)
            if key not in values:
                values[key] = []
                order.append(key)
            values[key].append(value)
        duration = group[-1][0] - group[0][0]
        print(f"press {number:2d}  ({duration:.2f}s)")
        for device, etype, code in order:
            print(
                f"          {device}  {code_name(etype, code):<22} "
                f"{EV.get(etype, etype):<7} code={code:<4} values={values[(device, etype, code)]}"
            )
        print()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="mode", required=True)
    sub.add_parser("caps", help="decode /proc/bus/input/devices on stdin")
    presses = sub.add_parser("presses", help="group a raw capture directory into presses")
    presses.add_argument("directory")
    presses.add_argument(
        "--gap",
        type=float,
        default=0.45,
        help="seconds of quiet that end a press (default: %(default)s)",
    )
    args = parser.parse_args()
    if args.mode == "caps":
        return cmd_caps(sys.stdin)
    return cmd_presses(args.directory, args.gap)


if __name__ == "__main__":
    sys.exit(main())
