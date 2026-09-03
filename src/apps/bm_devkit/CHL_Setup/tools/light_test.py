#!/usr/bin/env python3
"""
Live ADC readout, for the torch test.

Streams `chl read` continuously so you can watch the number move while you put a
light on the optical window. Answers two questions at once:

  1. Is the C-FLUOR actually driving the ADC?  The reading must move, a lot.
  2. Is there a divider between the probe and the ADC?  See below.

THE DIVIDER QUESTION. The project design called for a 1/3 resistor divider ahead
of the ADS1115; the firmware as built assumes the ADC sees the probe's output
directly. If the divider is fitted and the firmware does not know, every
concentration is 3x too low. A bright light saturates a chlorophyll fluorometer,
so the ceiling the reading hits tells you which it is:

  no divider   reading climbs to ~3.3 V and CLIPS (>= 26000 counts, the 3V3 rail)
  1/3 divider  reading tops out near 1.67 V (~13300 counts) and does NOT clip

Anything in between means something else is going on - say so rather than
guessing.

    ./light_test.py              # run until ctrl-C
    ./light_test.py --seconds 45
"""

import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing.  pip3 install pyserial")


def find_port():
    hits = [p for p in sorted(glob.glob("/dev/cu.usbmodem*"))
            if p.endswith("1") and "SPOT" not in p]
    if not hits:
        sys.exit("No mote serial port. Is the Dev Kit USB plugged in, and is the "
                 "Bristlemouth rail powered?")
    return hits[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--seconds", type=float, default=0, help="0 = until ctrl-C")
    args = ap.parse_args()

    port = args.port or find_port()
    s = serial.Serial(port, 115200, timeout=0.4)
    time.sleep(0.3)

    print(f"Reading the ADC on {port}. Ctrl-C to stop.\n")
    print("Put a light on the optical window and watch `counts`.\n")
    print(f"{'time':>8}  {'counts':>7}  {'volts':>9}  {'ug/L':>9}   bar")
    print("-" * 74)

    lo, hi = None, None
    t0 = time.time()
    try:
        while True:
            if args.seconds and time.time() - t0 > args.seconds:
                break
            s.reset_input_buffer()
            s.write(b"chl read\n")
            s.flush()
            deadline = time.time() + 1.2
            buf = b""
            while time.time() < deadline:
                buf += s.read(512)
                if b"counts:" in buf and b"\n" in buf.split(b"counts:")[-1]:
                    break
            for line in buf.decode("utf-8", "replace").splitlines():
                if not line.startswith("counts:"):
                    continue
                try:
                    counts = int(line.split("counts:")[1].split(",")[0])
                    volts = float(line.split("volts:")[1].split(",")[0])
                    ugl = float(line.split("chl:")[1].split("ug/L")[0])
                except (IndexError, ValueError):
                    continue
                lo = counts if lo is None else min(lo, counts)
                hi = counts if hi is None else max(hi, counts)
                # Bar scaled to the 3V3 rail, which is the real ceiling here.
                bar = "#" * min(40, max(0, int(40 * counts / 26400)))
                flag = "  *** CLIPPED AT THE SUPPLY RAIL ***" if "clipped" in line else ""
                print(f"{time.strftime('%H:%M:%S'):>8}  {counts:>7}  {volts:>9.5f}"
                      f"  {ugl:>9.3f}   {bar}{flag}")
                break
            time.sleep(0.25)
    except KeyboardInterrupt:
        pass

    if lo is None:
        print("\nNo readings came back. Is the rail up? Try `./chl status`.")
        return

    print("\n" + "-" * 74)
    print(f"counts seen: {lo} .. {hi}   (swing {hi - lo})")
    if hi - lo < 200:
        print("VERDICT: the reading barely moved. The C-FLUOR is probably not driving\n"
              "         the ADC - check the orange signal wire and the analog ground.")
    elif hi >= 25000:
        print("VERDICT: reached the 3V3 rail, so there is NO divider and the firmware's\n"
              "         calibration is applied to the right voltage. Correct as built.")
    elif 12000 <= hi <= 15000:
        print("VERDICT: topped out near 1/3 of the rail. A DIVIDER IS FITTED and the\n"
              "         firmware is NOT compensating - every concentration is 3x low.\n"
              "         Fix before deploying (see README, 'the divider question').")
    else:
        print(f"VERDICT: peaked at {hi} counts, which is neither the rail nor a clean\n"
              f"         1/3 of it. Meter the C-FLUOR orange wire and compare against\n"
              f"         the volts column before trusting any of this.")


if __name__ == "__main__":
    main()
