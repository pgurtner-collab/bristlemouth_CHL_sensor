#!/usr/bin/env python3
"""
Run the wiper underwater and measure what it actually costs.

Every wiper figure in the project docs was measured in AIR. Brush drag in water
is much higher, and if the servo stalls or the sweep truncates once submerged the
antifouling fails silently - the first evidence being fouled optics weeks into a
deployment. This is the test that catches that on a bench.

    ./wet_wiper_test.py                 # 5 cycles at the current settings
    ./wet_wiper_test.py --cycles 8
    ./wet_wiper_test.py --sweep-travel  # also try several chlTravelMs values

WHY --sweep-travel MATTERS. chlTravelMs is the time allowed for one leg. If the
servo is slower under load than that allows, the next command arrives mid-travel
and the sweep is cut short - leaving an unwiped arc, with nothing in the
telemetry to say so. Increasing travel time until the mean current stops falling
finds the point where the servo is comfortably arriving before it is re-commanded.

The rail must be up for the whole run, so disable the Spotter's bridge power
controller first:

    ./chl --spotter bridge cfg set 0 s u bridgePowerControllerEnabled 0
    ./chl --spotter bridge cfg commit 0 s

and RESTORE IT AFTERWARDS - leaving it off stops the temperature string.
"""

import argparse
import glob
import re
import statistics
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing.  pip3 install pyserial")

# Measured in air, 2026-09-03: full 600-2400 us range, chlTravelMs 1550, 2 sweeps.
AIR = {"duration_ms": 6234, "base_ma": 18, "mean_ma": 45, "peak_ma": 230}

BUS_V = 23.8            # measured
SOLAR_WH_DAY = 30.0     # Sofar's figure for daily average production

CYCLE_RE = re.compile(
    r"Cycle (\d+) (complete|ABORTED): (\d+) ms, base (\d+) mA, "
    r"mean (\d+) mA, peak (\d+) mA, n (\d+)")


def find_port():
    hits = [p for p in sorted(glob.glob("/dev/cu.usbmodem*"))
            if p.endswith("1") and "SPOT" not in p]
    if not hits:
        sys.exit("No mote serial port. Is the rail powered? Is the power "
                 "controller disabled?")
    return hits[0]


def send(ser, cmd):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()


def run_cycle(ser, timeout=40):
    """Trigger one wipe and return its parsed telemetry."""
    send(ser, "chl wipe")
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += ser.read(2048)
        text = buf.decode("utf-8", "replace")
        m = CYCLE_RE.search(text)
        if m:
            return {
                "n": int(m.group(1)), "ok": m.group(2) == "complete",
                "duration_ms": int(m.group(3)), "base_ma": int(m.group(4)),
                "mean_ma": int(m.group(5)), "peak_ma": int(m.group(6)),
                "samples": int(m.group(7)),
            }
        if "already running" in text or "not initialized" in text:
            return None
    return None


def set_key(ser, key, value):
    send(ser, f"chl set {key} {value}")
    time.sleep(2.5)
    ser.reset_input_buffer()


def summarise(label, runs):
    if not runs:
        print(f"  {label}: no cycles completed")
        return None
    med = {k: statistics.median([r[k] for r in runs])
           for k in ("duration_ms", "base_ma", "mean_ma", "peak_ma")}
    peaks = [r["peak_ma"] for r in runs]
    print(f"  {label:<18} n={len(runs)}  dur {med['duration_ms']:.0f} ms  "
          f"base {med['base_ma']:.0f}  mean {med['mean_ma']:.0f}  "
          f"peak {med['peak_ma']:.0f} mA  (peaks {min(peaks)}-{max(peaks)})")
    return med


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--cycles", type=int, default=5)
    ap.add_argument("--sweep-travel", action="store_true")
    ap.add_argument("--travel-values", default="1550,2000,2500,3000")
    args = ap.parse_args()

    ser = serial.Serial(args.port or find_port(), 115200, timeout=0.4)
    time.sleep(0.4)

    print("=" * 74)
    print("WET WIPER TEST")
    print("=" * 74)
    print(f"Air baseline for comparison: dur {AIR['duration_ms']} ms, "
          f"mean {AIR['mean_ma']} mA, peak {AIR['peak_ma']} mA\n")
    print("The wiper should be SUBMERGED before you continue.")
    try:
        input("Press Enter when it is under water (ctrl-C to abort): ")
    except KeyboardInterrupt:
        print("\naborted"); return

    print(f"\nRunning {args.cycles} cycles at the current settings...")
    runs = []
    for i in range(args.cycles):
        r = run_cycle(ser)
        if r is None:
            print(f"  cycle {i+1}: NO RESULT - is the servo rail up?")
            continue
        flag = "" if r["ok"] else "   *** ABORTED ***"
        print(f"  cycle {i+1}: {r['duration_ms']} ms, base {r['base_ma']}, "
              f"mean {r['mean_ma']}, peak {r['peak_ma']} mA{flag}")
        runs.append(r)
        time.sleep(1.5)

    print("\n" + "-" * 74)
    water = summarise("in water", runs)
    if water is None:
        return
    print(f"  {'in air (stored)':<18} "
          f"dur {AIR['duration_ms']} ms  base {AIR['base_ma']}  "
          f"mean {AIR['mean_ma']}  peak {AIR['peak_ma']} mA")

    dm = water["mean_ma"] / AIR["mean_ma"]
    dp = water["peak_ma"] / AIR["peak_ma"]
    print(f"\n  water/air:  mean x{dm:.2f}   peak x{dp:.2f}")

    if args.sweep_travel:
        print("\n" + "-" * 74)
        print("Varying chlTravelMs. Mean current should FALL as travel time rises;")
        print("where it stops falling is where the servo comfortably arrives.\n")
        original = 1550
        by_travel = {}
        for tv in [int(x) for x in args.travel_values.split(",")]:
            set_key(ser, "chlTravelMs", tv)
            rs = [r for r in (run_cycle(ser) for _ in range(3)) if r]
            by_travel[tv] = summarise(f"travel {tv} ms", rs)
            time.sleep(1)
        set_key(ser, "chlTravelMs", original)
        print(f"\n  restored chlTravelMs = {original}")

        best = None
        prev = None
        for tv in sorted(by_travel):
            m = by_travel[tv]
            if m is None:
                continue
            if prev is not None and m["mean_ma"] >= prev["mean_ma"] * 0.97:
                best = best or tv
            prev = m
        if best:
            print(f"  mean current stops improving around chlTravelMs = {best} ms")

    # --- power, recomputed with the measured water figures ---
    print("\n" + "=" * 74)
    print("POWER, recomputed with these measurements")
    print("=" * 74)
    extra_j = (water["mean_ma"] - water["base_ma"]) / 1000 * BUS_V * \
              (water["duration_ms"] / 1000)
    for interval_min, dur_s in ((10, 90), (20, 120)):
        cycles = 86400 / (interval_min * 60)
        wipe_wh = cycles * extra_j / 3600
        rail_wh = 0.739 * (dur_s / (interval_min * 60)) * 24
        total = rail_wh + wipe_wh
        print(f"  {dur_s:>3} s every {interval_min:>2} min: "
              f"wiper {wipe_wh:.3f} Wh/day, total {total:.2f} Wh/day "
              f"({100*total/SOLAR_WH_DAY:.1f}% of solar)")
    print(f"\n  energy per cycle in water: {extra_j:.2f} J "
          f"(air was 4.28 J)")

    # --- verdicts ---
    print("\n" + "=" * 74)
    stall = water["peak_ma"]
    print(f"chlStallMa is 450 mA; worst peak seen in water was {max(r['peak_ma'] for r in runs)} mA.")
    if max(r["peak_ma"] for r in runs) > 400:
        print("  -> TOO CLOSE. Raise chlTravelMs first (a slower sweep draws less);")
        print("     only raise chlStallMa if a slower sweep does not help.")
    elif max(r["peak_ma"] for r in runs) > 300:
        print("  -> Margin is thinner than in air. Watch the peak trend on the")
        print("     dashboard once deployed; a rising trend means the brush is loading up.")
    else:
        print("  -> Comfortable margin.")

    if any(not r["ok"] for r in runs):
        print("\nSOME CYCLES ABORTED - the servo could not be commanded. Investigate")
        print("before deploying; this is not a marginal-torque symptom, it is an I2C")
        print("or power fault.")

    med_dur = water["duration_ms"]
    expected = 4 * 1550
    if abs(med_dur - expected) > 300:
        print(f"\nDuration {med_dur:.0f} ms differs from the commanded {expected} ms.")
        print("The state machine is time-driven, so this points at a stalled loop.")


if __name__ == "__main__":
    main()
