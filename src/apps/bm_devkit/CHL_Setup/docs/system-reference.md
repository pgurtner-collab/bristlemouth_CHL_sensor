# CHL node — complete system reference

Turner Designs C-FLUOR chlorophyll fluorometer with an antifouling wiper, on a
Bristlemouth Dev Kit, reporting through a Sofar Spotter.

**Every claim below is tagged with where it comes from.** This system was built by
more than one person and the design documents do not everywhere match what was
actually assembled, so provenance matters more than tidiness:

| Tag | Meaning |
|---|---|
| **[M]** | Measured on this hardware, with the date |
| **[C]** | Stated in the firmware source or a code comment |
| **[D]** | From the design document (`bristlemouth-chla/CLAUDE.md`) — **may not match as-built** |
| **[S]** | Datasheet or vendor documentation |
| **[?]** | Unverified — nobody has confirmed this against the physical build |

---

## 1. Identifiers

| | Value | Source |
|---|---|---|
| CHL node ID | `aeac746f4fcd791b` | [M] |
| Mote MAC | `00:00:4f:cd:79:1b` | [M] |
| Mote console | `/dev/cu.usbmodem00004FCD791B1` (the one ending `1`) | [M] |
| Spotter console | `/dev/cu.usbmodemSPOT_32390C1` | [M] |
| Spotter | `SPOT-32390C`, "Kimberly's Reef" | [M] |
| Chain position | 3rd of 4 (three thermistors + this node) | [M] |
| Network config CRC | `0x4491bdef` (was `0xffb9b248` before this node) | [M 2026-09-03] |
| Dev Board revision | AE | [D] |
| App | `CHL_Setup`, BSP `bm_mote_v1_0` | [M] |

Chain order from `bm topo` [M]:

```
bridge | e3162c891a281b40 (1) | 52d58b687aa9cf4b (2) | aeac746f4fcd791b (CHL) | cf075cd8a3720042 (4)
```

The bottom thermistor self-reports as position **4** now, not 3. Kept deliberately.

---

## 2. Electrical

### 2.1 I2C bus — every device, confirmed present

`chl scan` [M 2026-09-03]:

| Address | Device | On the Dev Board? |
|---|---|---|
| `0x40` | PCA9685 16-channel PWM (servo driver) | added |
| `0x48` | ADS1115 16-bit ADC (Adafruit #1085), ADDR→GND | added |
| `0x41` | INA232 power monitor, PoDL (draw from the buoy) | on-board |
| `0x43` | INA232 power monitor, main | on-board |
| `0x70` | TCA9546A I2C mux | on-board |
| `0x76` | BME280 humidity/pressure | on-board |

The mux is left with channels 1 and 2 both enabled by `bristlefin.sensorsInit()`,
so external devices on channel 2 need no channel switching [C].

`0x48` was chosen for the ADS1115 because `0x4A` — the other obvious option —
collides with an on-board device on mux channel 1 [C].

### 2.2 Dev Board connections

Right-hand terminal blocks [D]:

| Terminal | Signal | Goes to |
|---|---|---|
| 1 or 12 | GND | common ground star point |
| 3 | VOUT (12 V) | C-FLUOR red |
| 6 | I2C SCL | ADS1115 SCL, PCA9685 SCL |
| 7 | I2C SDA | ADS1115 SDA, PCA9685 SDA |

0.1" header [D]:

| Pin | Signal | Goes to |
|---|---|---|
| H26 | 3V3 | ADS1115 VDD, PCA9685 VCC |
| H29 | IO1 | 5 V buck EN |
| H25 | GND | — |

**ADS1115 VDD is 3.3 V** — confirmed by Mark [M]. That is what limits the analog
input, not the PGA range: absolute maximum on an input is VDD + 0.3 V however
wide the range is set, so the real ceiling is ~3.3 V = 26,400 counts.

### 2.3 C-FLUOR pinout

Turner manual Appendix C [S]:

| Wire | Pin | Function | Connect to |
|---|---|---|---|
| Red | 1 | Supply 3–15 VDC | VOUT (12 V) |
| Black | 2 | Supply ground | GND |
| Orange | 3 | Signal out, 0–5 VDC | ADS1115 AIN0 |
| Green | 4 | Analog ground | GND star point |
| — | 5, 6 | unused | — |

Connector **MCIL-6-FS** (SubConn Micro) [S].

> ⚠️ Turner's own manual contradicts itself on the signal wire colour — §2.4's
> figure says **White**, Appendix C's table and Appendix F say **Orange**. Appendix C
> is authoritative. Continuity-check before connecting. [S]

> ⚠️ **>15 VDC destroys the probe.** VOUT is 12 V by default. Never wire it to
> VBUS (24 V). [S]

Draw: **≤22 mA at 12 VDC**, response **T99 under 0.5 s** [S]. The fast response is
why a short bus power window works at all.

### 2.4 ⚠️ THE DIVIDER QUESTION — unresolved, and it is a 3× error

The design document specifies a **1/3 divider** — three identical 10 kΩ metal-film
resistors — between the C-FLUOR's orange wire and AIN0, with `chlAdsPga = 2`
(±2.048 V) [D]. The ratio `R/(2R+R) = 1/3` is exact independent of R, so only
mismatch between the three matters.

**The firmware as built assumes no divider.** It applies the Turner calibration
directly to the ADC voltage, and runs `chlAdsPga = 1` (±4.096 V).

If the divider is fitted and the firmware does not know, **every concentration is
3× too low.**

Evidence that it is *not* fitted, none of it conclusive:

- Mark's "clipping above 3.3 V is fine" only makes sense without a divider — with
  ÷3 the probe would need to reach 10 V to clip the ADC [M]
- The reading sat stably at **0.0361 V** for hours; the certificate's blank offset
  is **0.0291 V**. Those are close. With ÷3 a blank would read 0.0097 V. [M]

**Settle it with `tools/light_test.py`** before trusting any data. A bright light
saturates the fluorometer, and the ceiling the reading hits discriminates:

| Peak counts | Meaning |
|---|---|
| ≥ 25,000 (clips at the 3V3 rail) | no divider — firmware is correct as built |
| ~13,300 (≈ 1/3 of the rail) | **divider fitted, firmware 3× low — fix before deploying** |
| < 500 swing | the C-FLUOR is not driving the ADC at all |

If a divider *is* fitted, the fix is config only, no reflash: set
`chlCalScale` to `24.6872 × 3 = 74.0616` and `chlCalOffsetV` to `0.0291 / 3 = 0.0097`.

### 2.5 ⚠️ Servo power — two documents disagree [?]

| Source | Says |
|---|---|
| [D] | Servo rail from an **external 5 V/6 V buck** (Pololu D24V22F5) off VOUT, EN driven by IO1, with a 4700 µF bulk cap for inrush |
| [C] | `bristlefin.enable5V()` — "powers the servo through the PCA9685's V+ terminal", i.e. the **Dev Board's own 5 V rail** |

The same design doc states the Dev Board 5 V rail is only 300 mA [D]. Measured
bus peak during a wipe is 241 mA at 23.8 V ≈ 5.3 W above idle [M], which at 5 V
would be roughly 1 A — over that limit.

It demonstrably works, and has for dozens of cycles with no stall or brownout
[M], so this is not urgent. But **nobody has verified which arrangement is
physically built**, and it matters for reliability. Trace it before deployment.

The firmware contains an unused helper to drive IO1 as a servo-rail enable
(BF_IO1 is hardcoded INPUT in a const struct upstream, so it needs a named
`PCA9535Pin_t` override) — relevant if the external-buck arrangement is the real
one.

### 2.6 PCA9685 ALL-CALL vs the I2C mux

Out of reset the PCA9685 answers to **0x70 in addition to its own address**, and
0x70 is the TCA9546A mux [S]. `TCA9546A::setChannel()` reads the mux back to
verify every change, so two devices would drive the bus at once [C].

`motorClearAllCall()` is the first thing user `setup()` does. But
`sensorsInit()` — which brings up the mux — runs *before* user setup, so there is
a window at every power-up that firmware cannot close. **The real fix is in
hardware: take the PCA9685's VCC from the switched 5 V rail rather than the
always-on 3V3, so the part is unpowered until `enable5V()` runs.** [C]

### 2.7 Servo

goBILDA Proton, steel gears [S]:

| | |
|---|---|
| Pulse width | 600–2400 µs |
| Travel | 177°, 0.098°/µs |
| Supply | 4.8–6.0 V |
| Current | 80–90 mA no load, 800–1000 mA stall |

Waterproofed by Mark to a mineral-oil-filled ROV protocol with a Rustoleum Leak
Seal coating.

**Measured on this build [M 2026-09-03]**, full 600–2400 µs range, 2 sweeps:

| | |
|---|---|
| Cycle duration | 6236 ms (4 legs × 1550 ms) |
| Bus current, baseline | 14–21 mA |
| Bus current, mean over cycle | 43–46 mA |
| Bus current, peak | **201–241 mA** |
| Current samples per cycle | ~613 |

Peak is *lower* on full-range sweeps than on the old 45–135° ones (245 mA),
because the longer travel time means gentler acceleration.

`chlStallMa` is set to **450 mA** — comfortably above the 241 mA observed, well
below the ~1 A stall.

---

## 3. Power budget

Reproduce: `python3 tools/power_budget.py`. Full write-up: `docs/power-budget.md`.

| | |
|---|---|
| Rail-on draw, node + C-FLUOR | **0.74 W** [M + S] |
| Energy per wipe cycle, above idle | **4.3 J** [M] |
| Dead time per power-on | **13.2 s** [M] |
| Spotter solar, daily average | ~30 Wh [S] |
| Spotter's own baseline | 250–300 mW [S] |

Dead time breakdown — this is what makes short windows wasteful:

| Stage | |
|---|---|
| Boot and rail bring-up | ~1.5 s (overlaps the wipe delay) |
| `chlFirstWipeDelayMs` | 2 s |
| Wipe, 2 full-range sweeps | 6.2 s |
| `chlSettleMs` | 3 s |
| Transmit margin | 2 s |

A window of D seconds yields about **D − 13.2** samples at 1 Hz.

**As deployed — 90 s every 10 min:** 76 samples per reported value, 2.83 Wh/day,
**9.4 % of average daily solar**, 144 packets/day, 6.9 kB/day.

**The wiper is not a power consideration at any interval.** It is 0.17 Wh/day —
6 % of even this option's own total. Always-on would be 17.9 Wh/day, of which
17.7 is simply holding the rail up.

**Dead time is paid per window, not per minute.** Buy a finer cadence from
`sampleIntervalMs`, never by splitting a window into subsamples.

---

## 4. Spotter / bridge configuration

Set from the **Spotter's** USB console only — the mote has no `bridge` command [M].

```
bridge cfg set 0 s u <key> <value>
bridge cfg commit 0 s
bridge cfg get 0 s <key>
```

### As deployed [M 2026-09-03]

| Key | Value | Effect |
|---|---|---|
| `bridgePowerControllerEnabled` | 1 | duty-cycle the Bristlemouth bus |
| `sampleIntervalMs` | 600000 | power the bus every 10 min |
| `sampleDurationMs` | 90000 | for 90 s |
| `subsampleEnabled` | 0 | one continuous window, not several short ones |
| `samplesPerReport` | 1 | send each aggregation immediately, unbatched |
| `transmitAggregations` | 1 | transmit them |

### The power controller is not optional to understand

**It drives everything.** `sensorController` discovers and subscribes sensor nodes
only after `_bridge_power_controller->waitForSignal(true, …)`, and aggregation
runs on the same timer [C]. So with `bridgePowerControllerEnabled = 0`:

- the bus stays powered — convenient for bench work
- **no topology sampling, no aggregation, and the temperature string stops
  reporting entirely** [M — this cost several hours of misdiagnosis]

Defaults in the source are 5 min 10 s every 30 min [C]; this buoy was set
differently. **Always read the buoy rather than trusting the defaults.**

`INIT_POWER_ON_TIMEOUT_MS` gives a 2-minute grace period after a Spotter reboot
before duty-cycling starts [C].

### Topology changes need no manual action

Verified [M 2026-09-03]. The bridge detects a changed configuration CRC,
registers it and uploads the new configuration by itself:

```
The smConfigurationCrc is not in the known list! calc: 0x4491bdef Adding it.
Got CRC 4491bdef OLD CRC 0
Updating CRC and topology in report builder!
Updated reportBuilders max network sensor type list
```

Known CRCs live in `smConfigurationCrc` in the bridge's **hardware** partition, up
to 9 of them [C]. It only runs this on a bridge init.

### `samplesPerReport` and the temperature string

Aggregations are timestamped every `sampleIntervalMs` but transmitted in batches
of `samplesPerReport` [C]. At the original 3, that meant three rows per sensor
arriving together — which is why temperature appeared to lag by an hour.

Now 1, so each aggregation goes out immediately. **Halving `sampleIntervalMs` for
the chlorophyll node doubles the temperature string's row count**, from 216 to
432/day. Changing a shared bus schedule changes the other payload's data rate.

### The transmit queue

`BmNetworkTypeCellularOnly` lands in `MS_Q_CELLULAR_ONLY`, which is shallow and
is where the Spotter puts its own multi-kilobyte payloads. Measured [M]:

```
cellular only : [MS] [ERROR] Queue MS_Q_CELLULAR_ONLY is full.
                [BM_TX] [ERROR] Unable to submit message to cell-only queue
with fallback : [MS] [INFO] Added message (len 76) to queue MS_Q_LEGACY
                [BM_TX] [INFO] Submitted spotter/transmit-data ... Len: 48
```

It is **intermittently** full, not permanently — an earlier cellular-only packet
did get through. Intermittent is worse: packets drop unpredictably and
`spotter_tx_data()` returns `BmOK` regardless, because that only means the publish
left the node. **Use the Iridium-fallback type** (`chlTxCellularOnly = 0`);
`MS_Q_LEGACY` accepted every attempt.

---

## 5. Firmware

### 5.1 Layout

```
src/apps/bm_devkit/CHL_Setup/
  user_code/
    user_code.cpp     setup/loop, aggregation, packet, transmit, CLI helpers
    chl_config.{h,cpp} the runtime config table
    chl_cli.cpp        the `chl` console command
    ads1115.{h,cpp}    ADC driver, runtime PGA and saturation threshold
    motor_code.{h,cpp} PCA9685 + servo + per-wipe current telemetry
    chl_app.h          the small interface the CLI needs
  tools/
    chl                serial console wrapper
    light_test.py      live ADC readout for the torch test
    power_budget.py    the trade study
  docs/
    system-reference.md  this file
    power-budget.md      schedule trade study
```

### 5.2 Runtime configuration

Every number that might need changing after the enclosure is closed. Persists in
the mote's user partition. `chl cfg` lists them live.

| Key | Default | Deployed | Meaning |
|---|---|---|---|
| `chlAggPeriodMs` | 600000 | 600000 | window after the first (needs reset) |
| `chlFirstWindowMs` | 150000 | **78000** | first window after boot — sized to fit the bus window (needs reset) |
| `chlFirstWipeDelayMs` | 8000 | 2000 | boot → first wipe |
| `chlSamplePeriodMs` | 1000 | 1000 | ADC read interval (needs reset) |
| `chlWipeIntervalMin` | 0 | **0** | EXTRA cycles while powered; 0 is normal |
| `chlWipeOnBoot` | 1 | 1 | one cycle per power-on — **this is the schedule** |
| `chlWipeSweeps` | 2 | 2 | out-and-back passes per cycle |
| `chlParkUs` | 1058 | **2400** | resting pulse width |
| `chlSweepUs` | 1973 | **600** | far-end pulse width |
| `chlTravelMs` | 800 | **1550** | time per leg |
| `chlSettleMs` | 3000 | 3000 | post-wipe settle before readings count |
| `chlStallMa` | 600 | **450** | peak current flagged as a stall |
| `chlPwrAddr` | 0x41 | 0x43 | which INA232 the wipe current comes from |
| `chlAdsPga` | 1 | 1 | 0=±6.144 1=±4.096 2=±2.048 3=±1.024 4=±0.512 5=±0.256 V |
| `chlSatCounts` | 26000 | 26000 | clip flag threshold — the **supply** rail, not the PGA rail |
| `chlCalOffsetV` | 0.0291 | 0.0291 | Turner blank offset, V |
| `chlCalScale` | 24.6872 | 24.6872 | Turner coefficient, µg/L per V |
| `chlTxCellularOnly` | 1 | **0** | 0 = allow Iridium fallback |
| `chlRawLog` | 0 | 0 | log every individual sample |

**`chlWipeOnBoot` and `chlWipeIntervalMin` are independent.** On a duty-cycled bus
uptime resets every window, so a free-running interval longer than the window can
never fire — the boot wipe *is* the schedule and the bus interval sets the wipe
interval. Conflating the two once disabled all wiping.

### 5.3 The packet — 47 bytes, little-endian, packed

| Off | Size | Type | Field |
|---|---|---|---|
| 0 | 1 | u8 | magic `0xC1` |
| 1 | 1 | u8 | version (1) |
| 2 | 1 | u8 | flags |
| 3 | 1 | u8 | ADS1115 PGA index |
| 4 | 4 | u32 | uptime, s |
| 8 | 2 | u16 | clean samples |
| 10 | 2 | u16 | discarded (wiper in the path) |
| 12 | 4 | f32 | mean, µg/L |
| 16 | 4 | f32 | min |
| 20 | 4 | f32 | max |
| 24 | 4 | f32 | std dev |
| 28 | 4 | f32 | **mean raw volts** |
| 32 | 2 | u16 | ADC read failures |
| 34 | 2 | u16 | samples at/near the rail |
| 36 | 2 | u16 | wipe count since power-on |
| 38 | 2 | u16 | wipe baseline current, mA |
| 40 | 2 | u16 | wipe mean current, mA |
| 42 | 2 | u16 | wipe peak current, mA |
| 44 | 2 | u16 | wipe duration, ms |
| 46 | 1 | u8 | checksum |

Flags: `0x01` ADC ready · `0x02` servo ready · `0x04` last wipe stalled · `0x08`
wipe in progress at window close · `0x10` no clean samples · `0x20` a reading hit
the rail · `0x40` no wipe has ever run.

**`volts_mean` is carried deliberately.** The calibration is two numbers off a
certificate; if the wrong ones were loaded, the whole deployment's µg/L would be
wrong with nothing to recover it from. Four bytes makes it recomputable — and
that is exactly what saved the readings taken before the correct certificate
values were entered.

**The wipe fields describe the most recent cycle and repeat in every packet**,
rather than appearing only in the window a wipe fell in. A single lost message
would otherwise lose the only evidence the wiper ran.

### 5.4 Build and flash

```bash
cd ~/Desktop/bristlemouth/bm_protocol/cmake-build/CHL_Setup
pixi run -e dev make -j8

printf 'bootloader\n' > /dev/cu.usbmodem00004FCD791B1
dfu-util -l | grep -c "Found DFU"        # expect 3
pixi run -e dev make dfu_flash
```

Success is **both progress bars completing**. The trailing
`dfu-util: Error during download get_status` is harmless. `make dfu_flash` prints
`Built target dfu_flash` even when the board was never in DFU mode — the bars are
the only real signal. Never interrupt a flash.

**The bus must be powered to flash.** With the power controller on you get 90 s
windows; disable it first:

```bash
./chl --spotter bridge cfg set 0 s u bridgePowerControllerEnabled 0
./chl --spotter bridge cfg commit 0 s
# ... flash ...
./chl --spotter bridge cfg set 0 s u bridgePowerControllerEnabled 1
./chl --spotter bridge cfg commit 0 s
```

> **Restoring the power controller is not optional.** Leaving it disabled stops
> the temperature string reporting and costs 17.9 Wh/day.

---

## 6. Console

`chl ...` commands go to the **mote's serial console**, not a shell. `tools/chl`
wraps that and finds the right port (the Dev Kit exposes two; only the one ending
`1` is the console).

```bash
cd src/apps/bm_devkit/CHL_Setup/tools
./chl                    # interactive
./chl status             # one command
./chl set <key> <value>
./chl --watch            # follow, filtered
./chl --spotter status   # the Spotter instead
./chl reset              # board commands pass through: reset, bm, gpio, info
```

| Command | |
|---|---|
| `chl status` | sensor, wiper, calibration, link |
| `chl cfg` | every config key with bounds and meaning |
| `chl read` | one conversion — counts, volts, µg/L |
| `chl scan` | probe every expected I2C device |
| `chl jog <us>` | drive to a pulse width and **hold** |
| `chl nudge <±us>` | relative move |
| `chl park` / `chl sweep` | drive to a stored endpoint |
| `chl release` | stop driving — do this after jogging |
| `chl setpark` / `chl setsweep` | store the current position, persist |
| `chl wipe [n]` | run a cycle now |
| `chl tx` | close the window and transmit now |

Zero-dependency fallback: `screen /dev/cu.usbmodem00004FCD791B1 115200`.

### Setting the wiper endpoints

They are pulse widths, not angles — where the brush sits is a fact about how the
horn was clocked on the spline, with no fixed relationship to servo degrees, and
it changes every time the brush is disturbed.

```bash
./chl release            # kill torque, position by hand
./chl jog 2400           # find the resting end
./chl setpark
./chl jog 600            # find the far end
./chl setsweep
./chl wipe               # watch a full cycle
./chl release
```

---

## 7. Data path

```
C-FLUOR ─analog→ ADS1115 ─I2C→ mote ─Bristlemouth→ Spotter ─cellular→ Sofar API
                                                                          │
                                              poller (10 min) ────────────┘
                                                    ↓
                                              SQLite on a Fly volume
                                                    ↓
                                   dashboard  +  daily CSV → git → Google Drive
```

### Sofar API

**`/api/sensor-data` is the ingestion path.** Measured against `/api/raw-messages`
on identical packet sets [M]:

| | `sensor-data` | `raw-messages` |
|---|---|---|
| Rows per request | no cap seen (1696 over 8 days) | **20, `limit` ignored** |
| Requests to catch up | 1 | 16 |
| Our packets | one row each, unwrapped | opaque hex behind a header byte |
| Node attribution | `bristlemouth_node_id` | none — scan for a magic byte |
| Position | 7 decimals | `approxLat` `"26.33"` — two |
| Timestamp | same clock as the temperature string | `transmitTime`, 2–3 s later |

Our packets appear as `data_type_name: binary_hex_encoded`, `sensorPosition: None`.

**Sofar's own dashboard cannot plot them.** Its panels are fixed and bound to
Sofar's `data_type_name` vocabulary. That has not changed and is why the custom
dashboard exists.

### Dashboard

<https://chl-kimberlys-reef.fly.dev/> — chlorophyll, wiper current trend,
measurement quality, platform telemetry, CSV export. Repo `markeleone/chl-dashboard`.

Viewers cost no Sofar API calls; the poller runs on a fixed 10-minute schedule
regardless of traffic.

### Backup

- `data/YYYY/YYYY-MM-DD.csv` committed daily by GitHub Actions at 06:20 UTC
- Each row keeps the message's **full raw hex**, so a decoder bug is recoverable
- Fly volume snapshots, 60-day retention
- Google Drive folder `1rUC5AwFDt2i0k8uN4OiwSUta_UHvD-OK`, once
  `RCLONE_CONFIG_GDRIVE` is set

~15 MB/year total.

---

## 8. Things that cost time, so they do not cost it again

1. **The Spotter power-cycles the bus.** A node averaging over a period longer
   than the power window transmits nothing, ever, while looking perfectly healthy
   on USB. `chlFirstWindowMs` exists for this.
2. **Disabling the power controller stops the temperature string.** Not the
   topology change, not the firmware. Restore it before diagnosing anything.
3. **`spotter_tx_data()` returning `BmOK` means the publish left the node** — not
   that the Spotter accepted it, queued it, or sent it. It is not delivery
   confirmation. Same for `spotter_log()`.
4. **The Spotter creates `bm/<node id>/` on its SD card only at boot.** A node
   that joins later logs to nowhere and everything else looks fine. Reset the
   Spotter after changing the chain.
5. **`BmErr` has `BmOK == 0`**, so `if (spotter_tx_data(...))` is true on
   *failure*. Upstream `rbr_coda_example` gets this backwards.
6. **Read the buoy, never the source defaults.** This Spotter's schedule differed
   from the compiled defaults in every field.
7. **Sample buffer sizing.** The RBR coda example assumes 2 Hz; a 16 Hz sensor
   undersizes it 8×.

---

## 9. Open items

| | Status |
|---|---|
| **The divider question (§2.4)** | **Unresolved — a 3× error if wrong. `tools/light_test.py` settles it.** |
| **C-FLUOR light test** | Not yet done. Nothing downstream is trustworthy until it is. |
| **Wiper endpoints in the sensor's frame** | 2400 = rest, 600 = sweep set; not yet verified against the optical face |
| Servo power path (§2.5) | Two documents disagree; as-built unverified |
| Temperature reporting | Not yet seen since the topology change; `samplesPerReport` now 1, watching |
| `chlStallMa` = 450 mA | Based on 241 mA peaks over dozens of cycles, not a stalled measurement |
| PCA9685 ALL-CALL window at boot | Firmware mitigates; the hardware fix is VCC on the switched 5 V rail |
| Bulk capacitor | Specified in [D]; presence unverified |
