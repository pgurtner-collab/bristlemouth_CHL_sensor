# Bridge power schedule — trade study

Reproduce with `python3 tools/power_budget.py`. Every electrical input is either
measured on this node or taken from a datasheet; the script names the source for
each so a number can be re-checked rather than trusted.

## Inputs

| Quantity | Value | Source |
|---|---|---|
| Bus voltage | 23.8 V | measured, INA232 0x43 |
| Node idle (mote + Bristlefin, rails up, servo released) | 18 mA (range 14.5–22.5) | measured 2026-09-03 |
| Wipe cycle, mean | 47 mA | measured, ~317 samples/cycle |
| Wipe cycle, peak | 242 mA | measured |
| Wipe duration | 3.22 s | measured (2 sweeps, 800 ms/leg) |
| C-FLUOR draw | ≤22 mA @ 12 V | Turner datasheet |
| C-FLUOR response, T99 | **<0.5 s** | Turner datasheet |
| Payload buck efficiency | 0.85 | assumed |
| Spotter solar, daily average | ~30 Wh | Sofar |
| Spotter own baseline | 250–300 mW | Sofar |
| Usable battery to BM loads | ~30 Wh of a 47 Wh pack | Sofar |

**Rail-on draw with the C-FLUOR attached: 0.74 W.** One wipe costs **2.2 J** above
that. Note that the 18 mA measurement was taken with the C-FLUOR *not connected* —
its contribution is the datasheet figure, not measured, and is the one input worth
re-checking once the sensor is wired.

## Dead time per power-on — 10.2 s

This is what sets the minimum useful window:

| Stage | Duration |
|---|---|
| First wipe delay (`chlFirstWipeDelayMs`) | 2.0 s |
| Wipe cycle | 3.2 s |
| Settle (`chlSettleMs`) | 3.0 s |
| Transmit margin before the rail drops | 2.0 s |
| **Total** | **10.2 s** |

The C-FLUOR's own warm-up does not appear because T99 is under 0.5 s — it is ready
long before the wiper has finished. Boot and rail bring-up is ~1.5 s and overlaps
the wipe delay.

So a window of duration D yields roughly `D − 10.2` samples at 1 Hz.

## Options

```
option                           duty  samples   Wh/day  %solar  wipes/yr  pkts/day  kB/day
A  always on, wipe/10 min      100.0%      589    17.82   59.4%    52,560       144     6.9
B  60 s every 20 min             5.0%       49     0.93    3.1%    26,280        72     3.5
C  as-found: 30 s every 5 min   10.0%       19     1.95    6.5%   105,120       288    13.8
D  120 s every 20 min           10.0%      109     1.82    6.1%    26,280        72     3.5
E  180 s every 30 min           10.0%      169     1.80    6.0%    17,520        48     2.3
```

## Conclusions

**The wiper is not a power consideration at any interval.** It is 0.03–0.18 Wh/day
in absolute terms and at most 9% of any option's own total. Option A spends 17.7 of
its 17.8 Wh/day on simply keeping the rail up. Whatever the wiper interval, the
schedule is what decides the power budget.

**The as-found configuration (C) is strictly dominated by D.** Identical duty cycle
and within 7% on energy, but:

- 19 samples per average instead of 109 — 2.4× worse noise for a mean
- 105,120 wiper cycles a year instead of 26,280 — 4× the servo and brush wear
- 288 packets a day instead of 72 — 4× the cellular traffic

C is not a power/quality trade-off. It is the same power for worse data, more wear
and more traffic, because four short windows cost four lots of the 10.2 s dead time
and buy four lots of the same measurement.

**Always on (A) is not viable.** 17.8 Wh/day is 59% of average daily solar and
sits above Sofar's own guidance for a sustainable Bristlemouth load even in the
tropics. It would run on a good day in summer and fall behind in a cloudy week.

**Recommended: D — 120 s every 20 min, subsampling off.**

```
bridge cfg set 0 s u sampleIntervalMs 1200000   # 20 min, unchanged
bridge cfg set 0 s u sampleDurationMs 120000    # 2 min continuous
bridge cfg set 0 s u subsampleEnabled 0         # one window, not four
bridge cfg set 0 s u bridgePowerControllerEnabled 1
bridge cfg commit 0 s
```

and on the mote:

```
chl set chlFirstWindowMs 110000     # close and transmit at 110 s, 10 s before the rail drops
chl set chlWipeIntervalMin 0        # one wipe per power-on via chlWipeOnBoot; uptime never
                                    # reaches a free-running interval
```

It costs the same as today, gives 5.7× the samples per reported value, quarters the
wiper wear, and quarters the data volume. If the sampling interval ever needs to be
finer than 20 minutes, take it from the interval rather than by splitting the window
— dead time is paid per window, not per minute.

## Effect on the temperature string

`sampleIntervalMs` is unchanged at 20 min in options B, D and E, so the aggregation
cadence — and therefore the temperature data rate of 3 rows per 20 min, 216 rows a
day — is unchanged. The nodes would get one continuous 120 s window to stream into
rather than four 30 s ones, which if anything gives the bridge more to average.

**Unverified:** exactly how the bridge aggregates soft-module data across subsample
windows is not documented, and no temperature has reached the API since 12:35 on
2026-09-03. Setting this and watching one 20-minute cycle is the decisive test, and
should be done before relying on it.

## Chain position

`bm topo` on 2026-09-03, after inserting the CHL node third:

```
bridge | e3162c891a281b40 (1) | 52d58b687aa9cf4b (2) | aeac746f4fcd791b (CHL) | cf075cd8a3720042 (4)
```

The last thermistor now self-reports as position **4**, not 3. Before the CHL node
was inserted the API returned `sensorPosition` 1, 2 and 3 for the three thermistors.
So anything keyed on "position 3 is the bottom thermistor" will silently shift, and
position 3 will be absent from temperature data.

**If that numbering matters, move the CHL node to the end of the chain**, after all
three thermistors. The thermistors keep 1, 2, 3 and the CHL node becomes 4. That is
a re-cable, not a config change, and it is the only way to keep the existing
numbering intact.
