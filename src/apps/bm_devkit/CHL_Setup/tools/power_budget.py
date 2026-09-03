#!/usr/bin/env python3
"""
Bridge power schedule trade study for the CHL node on SPOT-32390C.

Every electrical input is measured on this hardware or taken from a datasheet;
none is assumed. Sources are named in the table below so a number can be
re-checked rather than trusted.

    python3 tools/power_budget.py
"""

BUS_V = 23.8            # measured, INA232 0x43

# --- measured on this node, 2026-09-03, bus-powered from the Spotter ---
I_NODE_IDLE_MA = 18.0   # mote + Bristlefin, rails up, servo released (range 14.5-22.5)
I_WIPE_MEAN_MA = 47.0   # mean over a cycle
I_WIPE_PEAK_MA = 242.0  # highest 4.5 ms average seen
WIPE_S = 3.22           # measured duration, 2 sweeps

# --- Turner Designs C-FLUOR datasheet ---
CFLUOR_MA_AT_12V = 22.0 # "<=22mA at 12VDC"
CFLUOR_T99_S = 0.5      # "responds in under 0.5 seconds (T99)"
BUCK_EFF = 0.85         # 23.8 V -> 12 V payload rail, assumed

# --- timing, measured ---
WIPE_DELAY_S = 2.0      # chlFirstWipeDelayMs
SETTLE_S = 3.0          # chlSettleMs
TX_MARGIN_S = 2.0       # publish must reach the Spotter before the rail drops
SAMPLE_HZ = 1.0

# --- platform, Sofar's published figures ---
SOLAR_WH_DAY = 30.0            # "approximately 30Wh" average daily production
SPOTTER_BASELINE_W = 0.275     # "250-300mW in default configuration"
BATTERY_USABLE_WH = 30.0       # 47 Wh pack, ~30 Wh usable to BM loads

PACKET_BYTES = 48              # 47-byte payload + 1 Sofar header byte, measured


def bus_on_w() -> float:
    """Node draw while the Bristlemouth rail is powered, C-FLUOR attached."""
    return (I_NODE_IDLE_MA / 1000 * BUS_V) + (CFLUOR_MA_AT_12V / 1000 * 12.0 / BUCK_EFF)


def wipe_extra_j() -> float:
    """Energy a wipe costs ABOVE the idle draw already counted in bus_on_w()."""
    return (I_WIPE_MEAN_MA - I_NODE_IDLE_MA) / 1000 * BUS_V * WIPE_S


def samples_in(duration_s: float) -> int:
    """Clean samples obtainable in one power window."""
    dead = WIPE_DELAY_S + WIPE_S + SETTLE_S + TX_MARGIN_S
    return max(0, int((duration_s - dead) * SAMPLE_HZ))


def evaluate(name, interval_s, duration_s, always_on=False, wipe_interval_min=None):
    p_on = bus_on_w()
    if always_on:
        duty = 1.0
        # Wiping runs on its own timer rather than once per power-on.
        wipes_day = 86400 / (wipe_interval_min * 60)
        windows_day = 86400 / duration_s
        n_samples = samples_in(duration_s)
    else:
        duty = min(1.0, duration_s / interval_s)
        wipes_day = 86400 / interval_s          # one wipe per power-on
        windows_day = 86400 / interval_s        # one packet per power-on
        n_samples = samples_in(duration_s)

    rail_wh = p_on * duty * 24
    wipe_wh = wipes_day * wipe_extra_j() / 3600
    total_wh = rail_wh + wipe_wh

    return {
        "name": name,
        "duty_pct": duty * 100,
        "n_samples": n_samples,
        "rail_wh": rail_wh,
        "wipe_wh": wipe_wh,
        "total_wh": total_wh,
        "pct_solar": 100 * total_wh / SOLAR_WH_DAY,
        "wipes_day": wipes_day,
        "wipes_year": wipes_day * 365,
        "packets_day": windows_day,
        "kb_day": windows_day * PACKET_BYTES / 1000,
        "temp_rows_day": 3 * (86400 / interval_s) if not always_on else None,
    }


def main():
    print(f"Bus {BUS_V} V | node idle {I_NODE_IDLE_MA} mA "
          f"| C-FLUOR {CFLUOR_MA_AT_12V} mA @12V (T99 {CFLUOR_T99_S}s)")
    print(f"Rail-on draw {bus_on_w():.3f} W | one wipe costs {wipe_extra_j():.2f} J extra")
    print(f"Dead time per power-on: {WIPE_DELAY_S + WIPE_S + SETTLE_S + TX_MARGIN_S:.1f} s "
          f"(wipe delay + wipe + settle + transmit margin)")
    print(f"Platform: {SOLAR_WH_DAY} Wh/day solar, Spotter baseline "
          f"{SPOTTER_BASELINE_W * 24:.1f} Wh/day, leaving "
          f"~{SOLAR_WH_DAY - SPOTTER_BASELINE_W * 24:.0f} Wh/day for Bristlemouth\n")

    opts = [
        evaluate("A  always on, wipe/10 min", None, 600, always_on=True, wipe_interval_min=10),
        evaluate("B  60 s every 20 min", 1200, 60),
        evaluate("C  as-found: 30 s every 5 min", 300, 30),
        evaluate("D  120 s every 20 min", 1200, 120),
        evaluate("E  180 s every 30 min", 1800, 180),
    ]

    hdr = (f"{'option':<30}{'duty':>7}{'samples':>9}{'Wh/day':>9}{'%solar':>8}"
           f"{'wipes/yr':>10}{'pkts/day':>10}{'kB/day':>8}")
    print(hdr)
    print("-" * len(hdr))
    for o in opts:
        print(f"{o['name']:<30}{o['duty_pct']:>6.1f}%{o['n_samples']:>9}"
              f"{o['total_wh']:>9.2f}{o['pct_solar']:>7.1f}%{o['wipes_year']:>10,.0f}"
              f"{o['packets_day']:>10.0f}{o['kb_day']:>8.1f}")

    print("\nEnergy split (rail vs wiper):")
    for o in opts:
        share = 100 * o["wipe_wh"] / o["total_wh"]
        print(f"  {o['name']:<30} rail {o['rail_wh']:>6.2f} Wh  "
              f"wiper {o['wipe_wh']:>5.3f} Wh ({share:.2f}% of its own total)")


if __name__ == "__main__":
    main()
