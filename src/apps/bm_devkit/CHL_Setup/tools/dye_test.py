#!/usr/bin/env python3
"""
Guided dye titration — a linearity check for the C-FLUOR.

Walks through equal additions of a Rhodamine WT stock, averages a stable reading
at each step, and fits a line at the end. Records the YSI EXO's reading alongside
if you type it in, so the two instruments can be compared on shape.

    ./dye_test.py                       # defaults: 9.46 L bucket, 1 mL steps
    ./dye_test.py --volume-l 9.46 --step-ml 2 --steps 5

WHAT THIS DOES AND DOES NOT TELL YOU

Rhodamine WT is a SECONDARY standard here. The C-FLUOR's chlorophyll channel
excites around 460 nm and reads emission near 685 nm; Rhodamine WT excites near
555 nm and emits near 580 nm. The dye is seen through the tail of that overlap,
so the signal is real and proportional but the ug/L numbers are meaningless -
they are chlorophyll units applied to a dye.

  it DOES check: linearity, repeatability, drift, that the optics respond at all
  it does NOT check: the ug/L scale. That comes from the Turner certificate.

MAKE A STOCK FIRST. Bright Dyes FWT Red 25 is roughly 25 % active Rhodamine WT at
about 1.15 g/mL, so 1 mL of neat concentrate in 9.5 L is ~30,000 ppb. Rhodamine
fluorometry is linear to roughly 100 ppb and starts bending down above ~150 ppb
as the solution absorbs its own emission. Dosing neat concentrate measures that
roll-off, not the sensor.

  Stock: 1.0 mL concentrate into 1.000 L of water  (~288 mg/L)
  Then:  1 mL of stock into 9.46 L  ->  ~30 ppb per step

If the reading does not move after three additions, the cross-talk is weaker than
expected - stop, increase the step to 10 mL of stock, and start again from a
fresh bucket rather than continuing up a curve you have already bent.

PRACTICALITIES
  - Stir thoroughly and wait ~30 s after each addition. Unmixed dye reads high.
  - Keep the bucket out of direct sun; rhodamine photobleaches.
  - Park the wiper clear of the optical face (`chl park`, then `chl release`).
  - Rhodamine is temperature sensitive (~-2.6 %/degC). One bucket over one hour
    is fine; do not compare across sessions.
"""

import argparse
import glob
import statistics
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing.  pip3 install pyserial")

CONC_MG_PER_ML = 288.0   # Bright Dyes FWT Red 25, ~25 % active at ~1.15 g/mL
STOCK_DILUTION = 1000.0  # 1 mL concentrate into 1 L


def find_port():
    hits = [p for p in sorted(glob.glob("/dev/cu.usbmodem*"))
            if p.endswith("1") and "SPOT" not in p]
    if not hits:
        sys.exit("No mote serial port. Is the rail powered?")
    return hits[0]


def read_avg(ser, n=8):
    """Average n conversions, and report the spread so instability is visible."""
    vals = []
    for _ in range(n):
        ser.reset_input_buffer()
        ser.write(b"chl read\n")
        ser.flush()
        deadline = time.time() + 1.5
        buf = b""
        while time.time() < deadline:
            buf += ser.read(512)
            if b"counts:" in buf and b"\n" in buf.split(b"counts:")[-1]:
                break
        for line in buf.decode("utf-8", "replace").splitlines():
            if line.startswith("counts:"):
                try:
                    vals.append((int(line.split("counts:")[1].split(",")[0]),
                                 float(line.split("volts:")[1].split(",")[0])))
                except (IndexError, ValueError):
                    pass
                break
        time.sleep(0.2)
    if not vals:
        return None
    counts = [v[0] for v in vals]
    volts = [v[1] for v in vals]
    return {
        "counts": statistics.mean(counts),
        "volts": statistics.mean(volts),
        "sd": statistics.pstdev(counts) if len(counts) > 1 else 0.0,
        "n": len(counts),
    }


def fit(xs, ys):
    """Least squares slope, intercept, R^2."""
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return 0.0, my, 0.0
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    inter = my - slope * mx
    ss_res = sum((y - (slope * x + inter)) ** 2 for x, y in zip(xs, ys))
    ss_tot = sum((y - my) ** 2 for y in ys)
    r2 = 1 - ss_res / ss_tot if ss_tot else 0.0
    return slope, inter, r2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--volume-l", type=float, default=9.46, help="bucket volume, litres")
    ap.add_argument("--step-ml", type=float, default=1.0, help="mL of STOCK per addition")
    ap.add_argument("--steps", type=int, default=5)
    args = ap.parse_args()

    stock_mg_l = CONC_MG_PER_ML / (STOCK_DILUTION / 1000.0)
    ppb_per_step = args.step_ml / 1000.0 * stock_mg_l / args.volume_l * 1000

    # The plan is printed BEFORE the port is opened, so the dosing arithmetic can
    # be checked without the hardware connected - which is when you actually want
    # to check it.
    print("=" * 74)
    print("DYE TITRATION — linearity check")
    print("=" * 74)
    print(f"  bucket        {args.volume_l:.2f} L")
    print(f"  stock         1 mL concentrate in 1 L  (~{stock_mg_l:.0f} mg/L)")
    print(f"  step          {args.step_ml} mL of stock  ->  ~{ppb_per_step:.1f} ppb per addition")
    print(f"  final         ~{ppb_per_step*args.steps:.0f} ppb after {args.steps} additions")
    if ppb_per_step * args.steps > 150:
        print("\n  ⚠  The final concentration is past where Rhodamine WT stays linear")
        print("     (~150 ppb). Reduce --step-ml or --steps, or you will measure the")
        print("     solution bending rather than the sensor.")
    print("\n  Park the wiper clear of the optical face first:  chl park && chl release")
    print("  Stir well and wait ~30 s after each addition.\n")

    ser = serial.Serial(args.port or find_port(), 115200, timeout=0.4)
    time.sleep(0.4)

    rows = []
    for i in range(args.steps + 1):
        added = i * args.step_ml
        conc = i * ppb_per_step
        if i == 0:
            prompt = "Blank — sensor in clean water, no dye. Enter when ready: "
        else:
            prompt = (f"Add {args.step_ml} mL of stock (cumulative {added:g} mL, "
                      f"~{conc:.1f} ppb), stir, wait. Enter when ready: ")
        try:
            input(prompt)
        except KeyboardInterrupt:
            print("\nstopping early"); break

        r = read_avg(ser)
        if r is None:
            print("  no reading — is the rail up?")
            continue

        exo = input("  YSI EXO reading (any units, blank to skip): ").strip()
        try:
            exo_v = float(exo) if exo else None
        except ValueError:
            exo_v = None

        rows.append({"conc": conc, "counts": r["counts"], "volts": r["volts"],
                     "sd": r["sd"], "exo": exo_v})
        print(f"  -> {r['counts']:.0f} counts  ({r['volts']:.5f} V)  "
              f"sd {r['sd']:.1f} over {r['n']} reads\n")

    if len(rows) < 3:
        print("Need at least three points to say anything about linearity.")
        return

    print("=" * 74)
    print(f"{'ppb':>8} {'counts':>9} {'volts':>9} {'sd':>6} {'EXO':>9}")
    for r in rows:
        exo = f"{r['exo']:.3f}" if r["exo"] is not None else "-"
        print(f"{r['conc']:>8.1f} {r['counts']:>9.0f} {r['volts']:>9.5f} "
              f"{r['sd']:>6.1f} {exo:>9}")

    xs = [r["conc"] for r in rows]
    ys = [r["counts"] for r in rows]
    slope, inter, r2 = fit(xs, ys)
    print("\n" + "-" * 74)
    print(f"C-FLUOR:  counts = {slope:.2f} x ppb + {inter:.1f}    R^2 = {r2:.5f}")

    exo_rows = [r for r in rows if r["exo"] is not None]
    if len(exo_rows) >= 3:
        es, ei, er2 = fit([r["conc"] for r in exo_rows], [r["exo"] for r in exo_rows])
        print(f"YSI EXO:  reading = {es:.4f} x ppb + {ei:.3f}    R^2 = {er2:.5f}")
        print("\nCompare the R^2 values, not the slopes - different optics and path")
        print("lengths mean the slopes cannot agree and are not supposed to.")

    print()
    if r2 >= 0.995:
        print("VERDICT: linear. The sensor tracks concentration cleanly.")
    elif r2 >= 0.98:
        print("VERDICT: close to linear. Check whether the top point is bending -")
        print("         that is usually the solution self-absorbing, not the sensor.")
    else:
        print(f"VERDICT: R^2 = {r2:.4f} is poor. Before blaming the sensor, rule out")
        print("         incomplete mixing, the wiper sitting in the beam, an air")
        print("         bubble on the window, and over-concentration.")

    if len(rows) >= 3 and ys[-1] < ys[-2]:
        print("\n⚠  The last point read LOWER than the one before it. That is the")
        print("   classic inner-filter signature - the solution is too concentrated")
        print("   and is absorbing its own emission. Points above it are not usable.")


if __name__ == "__main__":
    main()
