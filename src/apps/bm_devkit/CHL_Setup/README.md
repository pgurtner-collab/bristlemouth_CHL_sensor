# CHL_Setup — Turner C-FLUOR chlorophyll sensor + antifouling wiper

Bristlemouth Dev Kit application for a Turner Designs C-FLUOR chlorophyll
fluorometer read through an ADS1115 ADC, with a goBILDA Proton servo wiper driven
by a PCA9685.

Node ID `aeac746f4fcd791b` · Spotter `SPOT-32390C` ("Kimberly's Reef")
Dashboard: **https://chl-kimberlys-reef.fly.dev/**

---

## 1. What it does

| | |
|---|---|
| ADC sample rate | once per second (`chlSamplePeriodMs`) |
| Reported value | mean of one window — see below, this is set by the bus schedule |
| Wiper | 2 full-range out-and-back sweeps, once per bus power-on (`chlWipeOnBoot`) |
| Transmitted | 47-byte binary packet per window, cellular with Iridium fallback |

### As deployed on SPOT-32390C, 2026-09-03

The Spotter duty-cycles the Bristlemouth bus, so the bus schedule — not
`chlAggPeriodMs` — is what sets the measurement cadence.

| Spotter (`bridge cfg … 0 s`) | | Mote (`chl set`) | |
|---|---|---|---|
| `sampleIntervalMs` | 600000 (10 min) | `chlFirstWindowMs` | 78000 |
| `sampleDurationMs` | 90000 (90 s) | `chlSamplePeriodMs` | 1000 |
| `subsampleEnabled` | 0 | `chlParkUs` | 600 |
| `bridgePowerControllerEnabled` | 1 | `chlSweepUs` | 2400 |
| `samplesPerReport` | 3 | `chlTravelMs` | 1550 |
| | | `chlWipeIntervalMin` | 0 (boot wipe only) |
| | | `chlStallMa` | 450 |

That yields **76 samples per reported value, 144 packets/day, 2.83 Wh/day
(9.4 % of average daily solar)**. The wiper sweeps the servo's full travel — more
of the window swept per cycle, and parked further off the optical face between
wipes, so less reflected light. Measured 6236 ms per cycle, 241 mA peak.

`chlWipeIntervalMin` is 0 and that is correct, not disabled: uptime resets every
power window, so a free-running interval longer than the window can never fire.
`chlWipeOnBoot` is the schedule, and the bus interval sets the wipe interval.
| Also logged | Dev Kit console; Spotter console; `bm/<node id>/chl_agg.log` on the Spotter SD card |

Concentration is `ug/L = (volts - chlCalOffsetV) * chlCalScale`, the standard
Turner two-constant form. This sensor head's certificate gives
**offset 0.0291 V, coefficient 24.6872 µg/L/V** (confirmed 2026-09-03).

`chlTxCellularOnly` defaults to 1 — no Iridium fallback. Iridium is billed per
message and this node sends 144 a day; that is not a fallback, it is a bill. If
cellular is down the packets still reach the Spotter's SD card for recovery on
retrieval. Set it to 0 before any offshore deployment.

### Sampling around the wiper

The sensor keeps sampling while the wiper moves, but those samples are **excluded
from the reported mean** and counted separately as `n_dirty`. The brush crosses
the optical path, so a reading taken then is not a measurement of anything. The
check is made both before and after each conversion, because a cycle can start or
finish while one is in flight.

Readings stay excluded for `chlSettleMs` (default 3 s) after the wiper stops, to
let the water in front of the window settle.

---

## 2. Runtime configuration

Every tunable is a config key in the mote's user partition. It survives reboot,
and **nothing here needs a reflash to change** — which matters because reflashing
needs the USB port and the USB port needs the buoy on a bench.

```
chl cfg                          # list every key, its value, bounds and meaning
chl set chlWipeIntervalMin 20    # set one key and save it to NVM
```

`chlAggPeriodMs` and `chlSamplePeriodMs` size a heap buffer at boot, so those two
need a `reset` afterwards. Everything else applies immediately.

The same keys are reachable over Bristlemouth from the Spotter's console, which
is how to reconfigure without opening the Dev Kit enclosure:

```
bm cfg set aeac746f4fcd791b u u chlWipeIntervalMin 60
bm cfg commit aeac746f4fcd791b u
```

### The bus schedule is the measurement schedule

`sampleDurationMs` bounds everything the node can do in one power-on, and 13.2 s
of every window is dead time before a clean sample exists:

| Stage | |
|---|---|
| Boot and rail bring-up | ~1.5 s (overlaps the wipe delay) |
| `chlFirstWipeDelayMs` | 2 s |
| Wipe, 2 full-range sweeps | 6.2 s |
| `chlSettleMs` | 3 s |
| Transmit margin before the rail drops | 2 s |

**Dead time is paid per window, not per minute.** Buy a finer cadence from
`sampleIntervalMs`, never by splitting a window into subsamples — four 30 s
subsamples cost the dead time four times and buy four copies of one measurement.
See `docs/power-budget.md`.

`samplesPerReport` (3 here) batches the *temperature* string's aggregations, so
halving `sampleIntervalMs` doubles that string's row count. Changing the bus
schedule for the chlorophyll node changes the temperature node's data rate too.

### Changing the wipe interval on a deployed buoy

**Not remotely, over cellular.** Sofar's API is read-only for data delivery;
there is no documented path for pushing a command to a Bristlemouth node from the
dashboard or the API. Reconfiguration needs a console — USB into either the
Spotter or the Dev Kit.

What that means in practice: set the interval before deployment, and choose it so
you will not want to change it. 30 min is the default. If the interval matters
enough to want in-water control, the honest answer is to raise it with Sofar
rather than to work around it in this firmware.

---

## 2a. Getting to the console

`chl ...` commands are typed at the **mote's USB serial console**, not in a
shell. `tools/chl` in this directory wraps that:

```bash
cd src/apps/bm_devkit/CHL_Setup/tools

./chl                       # interactive: type `status`, `cfg`, `jog 1100`, `help`
./chl status                # one command, print the reply, exit
./chl set chlWipeIntervalMin 20
./chl --watch               # follow the console, filtered to CHL lines and errors
./chl --watch --all         # follow everything
./chl --spotter status      # talk to the Spotter's console instead
./chl reset                 # the board's own commands work too: reset, bm, gpio, info
```

It finds the right port on its own — the Dev Kit exposes two USB serial ports and
only the one whose name ends in `1` is the console. Needs `python3` and
`pip3 install pyserial`, nothing else.

Zero-dependency fallback, if you would rather have a raw terminal:

```bash
screen /dev/cu.usbmodem00004FCD791B1 115200      # ctrl-a k to quit
```

## 3. Setting the wiper endpoints

The endpoints are stored as **servo pulse widths**, not angles. Where the brush
sits on the optical window is a fact about how the horn ended up clocked on the
spline, and it changes every time the brush or horn is disturbed. There is no
angle to calculate; you jog and look.

```bash
./chl release          # kill torque so the arm can be positioned by hand
./chl jog 1100         # drive to a pulse width and HOLD there
./chl nudge -25        # step relative
./chl setpark          # store the current position as the parked end
./chl jog 1950
./chl setsweep         # store the current position as the far end
./chl wipe             # run a full cycle and watch it
./chl release          # done - do not leave it holding torque
```

Both endpoints are clamped to the servo's 600–2400 µs spec range. Start with the
horn off if you are unsure — the *mechanical* limit with a wiper arm fitted can
be tighter than the electrical range, and driving into a hard stop is what stalls
the servo.

---

## 4. Wiper health telemetry

Every cycle is measured, not assumed. `motorService()` samples the INA232 bus
monitor on each pass of the main loop while the servo is moving — about 316
readings over a default 3.2 s cycle — and records baseline, mean, peak and the
*measured* duration.

Those figures ride in **every** transmitted packet, describing the most recent
cycle, rather than only in the window a wipe happened to land in. Wipes are 30
minutes apart and windows are 10 minutes, so tying the telemetry to one window
would mean a single lost message destroys the only evidence the wiper ran.

Measured on the bench, 2026-09-03, at 23.6 V bus:

| | |
|---|---|
| Node idle | 21–22 mA (0.50 W) |
| Wipe cycle mean | 53–56 mA |
| **Wipe cycle peak** | **248–251 mA (5.9 W instantaneous)** |
| Cycle duration | 3220 ms (commanded 4 × 800 ms) |
| Extra energy per cycle | ~2.4 J |
| At 10 min intervals | 144 cycles/day = **0.1 Wh/day** |
| Overnight (12 h dark) | 72 cycles = **0.05 Wh** |

Against Sofar's own figures for a Spotter — ~30 Wh/day of solar, ~30 Wh of usable
battery — the wiper is 0.3 % of the daily budget and 0.16 % of the battery
overnight. It cannot draw the buoy down. What a 10-minute interval actually
spends is **servo and brush life**: ~4,300 cycles a month, and the wear limit on a
hobby servo's gears and potentiometer is much less well characterised than its
current draw. That, not power, is the reason to lengthen the interval if it ever
needs lengthening.

A cycle whose peak exceeds `chlStallMa` (default 600 mA) sets the stall flag in
the packet and prints a warning. 600 mA is a placeholder chosen to sit between
the measured 250 mA free sweep and the Proton's ~1 A stall spec; tighten it once
there are enough real cycles to know the spread.

There is deliberately **no pub/sub topic** for this. `bm pub`/`bm sub` work
node-to-node on the Bristlemouth network and are useful on the bench, but the
Spotter only forwards `spotter/printf`, `spotter/fprintf` and
`spotter/transmit-data` to the cloud. A custom topic would never leave the buoy.
The packet is the only route to remote wiper QC.

---

## 5. Packet layout

47 bytes, little-endian, packed. Authoritative definition is `CHL_PACKET_LAYOUT`
in `user_code/user_code.cpp`; the decoder is `chl_packet.py` in the dashboard
repo and the two must be kept in step.

| off | size | type | field |
|---|---|---|---|
| 0 | 1 | u8 | magic `0xC1` |
| 1 | 1 | u8 | version `1` |
| 2 | 1 | u8 | flags |
| 3 | 1 | u8 | ADS1115 PGA index |
| 4 | 4 | u32 | uptime_s |
| 8 | 2 | u16 | n_clean |
| 10 | 2 | u16 | n_dirty |
| 12 | 4 | f32 | chl_mean, µg/L |
| 16 | 4 | f32 | chl_min |
| 20 | 4 | f32 | chl_max |
| 24 | 4 | f32 | chl_std |
| 28 | 4 | f32 | volts_mean, raw differential volts |
| 32 | 2 | u16 | adc_err |
| 34 | 2 | u16 | sat_count |
| 36 | 2 | u16 | wipe_count |
| 38 | 2 | u16 | wipe_base_ma |
| 40 | 2 | u16 | wipe_mean_ma |
| 42 | 2 | u16 | wipe_peak_ma |
| 44 | 2 | u16 | wipe_dur_ms |
| 46 | 1 | u8 | checksum, sum of bytes 0..45 |

Flags: `0x01` ADC ok · `0x02` servo driver ok · `0x04` last wipe stalled ·
`0x08` wipe in progress at window close · `0x10` window had no clean samples ·
`0x20` a reading hit the ADC rail · `0x40` wiper has never run.

**`volts_mean` is carried alongside `chl_mean` on purpose.** The Turner
calibration is two numbers copied off a certificate. If the wrong ones were
loaded, a whole deployment of µg/L would be wrong with nothing left to recover it
from. Four bytes buys the ability to recompute after the fact.

The magic byte and checksum exist because of what the receiving end looks like:
`/api/raw-messages` is an undifferentiated stream of every message the buoy sent,
each an opaque hex blob with a leading Sofar header byte we do not control.
Nothing says which entries are ours, so the decoder scans for `C1 01` and
confirms the checksum.

---

## 6. Bench commands

```
chl status      sensor, wiper, calibration and link state, plus a live reading
chl cfg         config table
chl set K V     set and persist one key
chl read        one conversion: raw counts, volts, µg/L
chl scan        I2C presence check for every device this app needs
chl jog N       drive the servo to N µs and hold
chl nudge ±N    relative move
chl park        drive to the stored park endpoint
chl sweep       drive to the stored sweep endpoint
chl release     stop driving the servo
chl setpark     store current position as park (optional explicit µs)
chl setsweep    store current position as sweep
chl wipe [n]    run a cleaning cycle now
chl tx          close the averaging window early and transmit
```

`chl read` prints **raw counts** as well as volts. This is the only way to tell a
genuinely high reading from a clipped one, which matters — see §8.

---

## 7. Build and flash

The app builds inside a checkout of Sofar's `bm_protocol` at v0.13.12-rc.1 or
later. Symlink it in rather than copying:

```bash
ln -sfn ~/Desktop/bristlemouth_CHL_sensor/src/apps/bm_devkit/CHL_Setup \
        ~/Desktop/bristlemouth/bm_protocol/src/apps/bm_devkit/CHL_Setup

cd ~/Desktop/bristlemouth/bm_protocol
mkdir -p cmake-build/CHL_Setup && cd cmake-build/CHL_Setup
pixi run -e dev cmake ../.. \
  -DCMAKE_TOOLCHAIN_FILE=../../cmake/arm-none-eabi-gcc.cmake \
  -DBSP=bm_mote_v1.0 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_APP_TYPE=BMDK -DAPP=CHL_Setup
pixi run -e dev make -j8
```

Flash over USB DFU — no debugger needed on Dev Board rev AD or later:

```bash
printf 'bootloader\n' > /dev/cu.usbmodem00004FCD791B1
dfu-util -l | grep -c "Found DFU"      # expect 3
pixi run -e dev make dfu_flash
```

Success is **both the Erase and Download progress bars completing**.
`make dfu_flash` prints `[100%] Built target dfu_flash` even when the board was
never in DFU mode, and the trailing `Error during download get_status` is
harmless. Never interrupt a flash.

---

## 8. Open items

1. **The C-FLUOR was not driving the ADC on the bench, 2026-09-03.** The ADS1115
   read 293–307 counts (~0.037 V, "0.35 µg/L") and did not change when the 12 V
   payload rail was switched off with `gpio clr bf_pl_buck_en`. A powered sensor's
   output would have collapsed to zero. During a wipe the same input jumped to
   1203 counts, which is what a floating high-impedance input does when a servo
   runs next to it. Conclusion: the analog path was not connected to a live
   sensor at that moment, so no reading through this firmware has yet been a real
   chlorophyll measurement. **Check with `chl read` and a torch on the optical
   window before trusting anything.**

2. ~~Calibration constants unverified.~~ **Resolved 2026-09-03**: offset
   0.0291 V, coefficient 24.6872 µg/L/V, from this head's certificate. The values
   the first version carried (0.0235 / 25.4520) were close but belonged to a
   different unit, so anything logged before this date is ~3 % off in slope.
   Recomputable from the `volts_mean` field in every packet.

3. ~~ADS1115 supply headroom.~~ **Resolved 2026-09-03**: VDD is confirmed 3V3,
   and clipping above 3.3 V is accepted — with this calibration that is ~79.5
   µg/L, far above anything this water produces. The ±4.096 V PGA range is kept
   because it is the widest that still resolves the full 0–3.3 V the hardware can
   see, at 125 µV/count. `chlSatCounts` is set to 26,000 (~3.25 V) rather than
   the 32,000 that the PGA range would imply, because **the supply is what limits
   the input, not the selected range** — watching for 32,000 counts on a 3V3
   supply would mean a signal pinned against the rail never raised a flag.

4. **`chlStallMa` is a placeholder** (600 mA) until enough real cycles exist.

5. **Topology changes need no action — verified 2026-09-03.** Inserting this node
   third in the chain changed the network configuration CRC from `0xffb9b248` to
   `0x4491bdef`. The bridge detects that itself, registers the new CRC
   (`smConfigurationCrc` in its hardware partition) and uploads the new
   configuration:

   ```
   The smConfigurationCrc is not in the known list! calc: 0x4491bdef Adding it.
   Got CRC 4491bdef OLD CRC 0
   Updating CRC and topology in report builder!
   Updated reportBuilders max network sensor type list
   ```

   But it only runs that on a bridge init, and `sensorController` subscribes the
   soft modules only after `_bridge_power_controller->waitForSignal(true, …)`.
   **With `bridgePowerControllerEnabled = 0` for bench work, no topology sampling
   or aggregation happens at all, and the temperature string stops reporting** -
   `OLD CRC 0` shows the report builder holding no topology whatever. Restore the
   power controller before concluding anything about missing temperature data.

6. **PCA9685 ALL-CALL vs the I2C mux.** Out of reset the PCA9685 also answers to
   0x70, which is the TCA9546A mux. `setup()` clears it first thing, but
   `sensorsInit()` runs before user code and has already talked to the mux by
   then, so firmware cannot close the window at boot. The fix is a wiring change:
   take the PCA9685's VCC from the switched 5 V rail rather than the always-on
   3V3, so the part is unpowered until `enable5V()`.

7. **The board's own `power |` log lines are noisier than before.** `setup()`
   reconfigures both INA232s from 256× averaging to 16× so a wipe's current peak
   is not averaged into nothing. The quantisation floor is unchanged at 250 µA per
   LSB; only the random noise went up.
