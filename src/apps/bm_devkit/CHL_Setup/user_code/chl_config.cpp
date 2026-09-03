#include "chl_config.h"
#include "bm_config.h"
#include "configuration.h"
#include "debug.h"
#include "motor_code.h"
#include <stdlib.h>
#include <string.h>

chlConfig_t chlCfg = {};

/* Every entry carries its own bounds. The point is not tidiness: these values
 * can be set remotely over Bristlemouth, where a typo cannot be seen and undone
 * the way it can on a bench. A sweep endpoint outside the servo's travel or a
 * sample period of zero would be a stalled servo or a wedged loop on a mooring
 * nobody can reach.
 *
 * NOTE: aggPeriodMs and samplePeriodMs size a heap buffer at boot, so changes to
 * those two only take effect after a reset. Everything else applies live. */
static const chlCfgEntry_t kTable[] = {
    /* 10 minutes, not the 60 s the PAR node uses. Two reasons, both learned from
     * the receiving end rather than guessed:
     *
     * /api/raw-messages returns at most 20 messages per request and ignores a
     * limit parameter, so every packet is a page to walk through on the cloud
     * side. At 60 s that is 1440 packets a day against the buoy's own ~110, and
     * a poller that spends most of its budget paging.
     *
     * And a 60 s window is finer than the signal deserves. Sofar's own smart
     * mooring temperature string reports at 20 min. 10 min still resolves a diel
     * cycle comfortably, averages 600 samples into each value instead of 60, and
     * cuts cellular traffic by a factor of ten. */
    {"chlAggPeriodMs", CHL_CFG_UINT, "ms",
     "averaging + transmit window (needs reset)",
     {.u = 600000}, {.u = 5000}, {.u = 3600000}},

    /* The FIRST averaging window after boot closes early, at this instead of
     * chlAggPeriodMs.
     *
     * This exists because the Spotter does not leave the Bristlemouth bus
     * powered. Its bridge power controller duty-cycles it - by default 5 min 10 s
     * of power every 30 min - so a node's uptime resets every cycle and it never
     * survives long enough for a 10 minute window to close. A node written to
     * average over a period longer than the power window transmits nothing, ever,
     * while looking perfectly healthy on a bench where USB keeps it alive.
     *
     * A short first window means one packet always gets out per power-on,
     * whatever the bus schedule is set to and without this firmware needing to
     * know it. On a bus that IS left on continuously, the effect is just one
     * quick packet at boot followed by the normal chlAggPeriodMs cadence.
     *
     * Keep it comfortably inside the Spotter's sampleDurationMs: boot, wipe and
     * settle burn the first ~15 s, and the packet has to be handed to the Spotter
     * before the rail drops. */
    /* 25 s, sized to fit inside a 30 s subsample window - the tightest schedule
     * this buoy is actually configured for (SPOT-32390C, measured 2026-09-03:
     * sampleInterval 20 min, sampleDuration 15.5 min, subsample 30 s every
     * 5 min, so the longest continuous power-on is 30 SECONDS).
     *
     * Deliberately sized for the worst case rather than the expected one. Too
     * short costs a thin first reading on a bus that is left powered; too long
     * costs every reading on a bus that is not. Those are not comparable
     * mistakes. */
    {"chlFirstWindowMs", CHL_CFG_UINT, "ms",
     "first window after boot closes at this, so a duty-cycled bus still yields data",
     {.u = 25000}, {.u = 10000}, {.u = 3600000}},

    /* 2 s, not 8. In a 30 s power window every second before the wipe is a
     * second of clean sampling lost: wipe (3.2 s) plus settle (3 s) already eats
     * a fifth of it. */
    {"chlFirstWipeDelayMs", CHL_CFG_UINT, "ms",
     "delay from boot to the first wiper cycle",
     {.u = 2000}, {.u = 500}, {.u = 120000}},

    {"chlSamplePeriodMs", CHL_CFG_UINT, "ms",
     "how often the ADC is read (needs reset)",
     {.u = 1000}, {.u = 100}, {.u = 60000}},

    /* 10 minutes. The power cost is genuinely negligible - a cycle is ~2.4 J, so
     * 144 a day is 0.1 Wh against a ~30 Wh/day solar budget, and an overnight
     * 12 hours of wiping is 0.05 Wh out of a ~30 Wh usable battery. What this
     * interval actually spends is servo and brush life: 144 cycles a day is
     * ~4,300 a month, and the wear limit on a hobby servo's gears and pot is far
     * less well characterised than its current draw. */
    {"chlWipeIntervalMin", CHL_CFG_UINT, "min",
     "minutes between wiper cycles, 0 disables wiping",
     {.u = 10}, {.u = 0}, {.u = 1440}},

    {"chlWipeSweeps", CHL_CFG_UINT, "count",
     "out-and-back passes per cycle",
     {.u = 2}, {.u = 1}, {.u = 20}},

    {"chlParkUs", CHL_CFG_UINT, "us",
     "servo pulse width at the parked (off-window) end",
     {.u = MOTOR_DEFAULT_PARK_US}, {.u = MOTOR_MIN_PULSE_US}, {.u = MOTOR_MAX_PULSE_US}},

    {"chlSweepUs", CHL_CFG_UINT, "us",
     "servo pulse width at the far end of the sweep",
     {.u = MOTOR_DEFAULT_SWEEP_US}, {.u = MOTOR_MIN_PULSE_US}, {.u = MOTOR_MAX_PULSE_US}},

    {"chlTravelMs", CHL_CFG_UINT, "ms",
     "time allowed for one leg of travel",
     {.u = MOTOR_DEFAULT_TRAVEL_MS}, {.u = 100}, {.u = 10000}},

    {"chlSettleMs", CHL_CFG_UINT, "ms",
     "after a wipe, how long before readings count as clean again",
     {.u = 3000}, {.u = 0}, {.u = 60000}},

    {"chlStallMa", CHL_CFG_UINT, "mA",
     "peak wipe current above which the cycle is flagged as stalled",
     {.u = 600}, {.u = 10}, {.u = 5000}},

    /* 0x43 (MAIN).
     *
     * Corrected 2026-09-03 after measuring both with the Dev Kit powered from the
     * Spotter's Bristlemouth bus, which is the deployed configuration: 0x43 reads
     * 22 mA at rest and 54 mA during a wipe, while 0x41 sits at a flat -6 mA and
     * does not respond to the servo at all. On a bench supply earlier the same
     * day it was the other way round, which is why this is a config key - but the
     * default should match the way the thing actually gets deployed.
     *
     * If a wipe reports no current rise, the firmware says so and names this key. */
    {"chlPwrAddr", CHL_CFG_UINT, "i2c",
     "INA232 read for wipe current: 67 = 0x43 main (correct on bus power), 65 = 0x41 podl",
     {.u = 0x43}, {.u = 0x40}, {.u = 0x4F}},

    {"chlAdsPga", CHL_CFG_UINT, "idx",
     "ADS1115 range: 0=+-6.144 1=+-4.096 2=+-2.048 3=+-1.024 4=+-0.512 5=+-0.256 V",
     {.u = 1}, {.u = 0}, {.u = 5}},

    {"chlRawLog", CHL_CFG_UINT, "bool",
     "1 = log every individual ADC sample, not just the window",
     {.u = 0}, {.u = 0}, {.u = 1}},

    {"chlWipeOnBoot", CHL_CFG_UINT, "bool",
     "1 = run one wiper cycle shortly after startup",
     {.u = 1}, {.u = 0}, {.u = 1}},

    /* Counts at which a reading is called clipped.
     *
     * NOT the PGA's own full scale. The ADS1115 runs from the 3V3 rail here, and
     * an analog input cannot go above VDD + 0.3 V whatever range is selected, so
     * the real ceiling is ~3.3 V = 26,400 counts, not the 32,767 the +-4.096 V
     * range implies. Watching for 32,000 would mean a signal pinned against the
     * supply never raised a flag at all.
     *
     * Clipping above 3.3 V is accepted for this deployment: with the C-FLUOR's
     * calibration that is ~81 ug/L, far above anything this water will produce.
     * The flag exists to make it visible if that assumption is ever wrong. */
    {"chlSatCounts", CHL_CFG_UINT, "counts",
     "reading is flagged as clipped at or above this; 26000 ~= 3.25 V on a 3V3 supply",
     {.u = 26000}, {.u = 1000}, {.u = 32767}},

    /* 0 - use BmNetworkTypeCellularIriFallback. Do not set this to 1 without
     * re-testing on the buoy.
     *
     * It was 1 for a few hours, on the reasoning that Iridium is billed per
     * message and 144 packets a day should not risk falling back to it. That
     * reasoning was fine and the conclusion was wrong: BmNetworkTypeCellularOnly
     * lands in MS_Q_CELLULAR_ONLY, which on SPOT-32390C is shallow and is where
     * the Spotter puts its own multi-kilobyte payloads. Measured 2026-09-03:
     *
     *   cellular only : [MS] [ERROR] Queue MS_Q_CELLULAR_ONLY is full.
     *                   [BM_TX] [ERROR] Unable to submit message to cell-only queue
     *   with fallback : [MS] [INFO] Added message (len 76) to queue MS_Q_LEGACY
     *                   [BM_TX] [INFO] Submitted spotter/transmit-data ... Len: 48
     *
     * Every packet would have been dropped, and the mote would have reported
     * success for all of them - spotter_tx_data() returns BmOK once the publish
     * leaves the node, whatever the Spotter then does with it.
     *
     * The Iridium concern is real but bounded: fallback only engages when
     * cellular is unavailable, and the packet is 47 bytes. Manage it with the
     * packet size and cadence, not by choosing a queue that does not work. */
    {"chlTxCellularOnly", CHL_CFG_UINT, "bool",
     "1 = cellular only - REJECTED by this Spotter, its queue is full; 0 = with fallback",
     {.u = 0}, {.u = 0}, {.u = 1}},

    /* Turner Designs C-FLUOR calibration, from this sensor head's certificate:
     *     ug/L = (measured volts - offset) * coefficient
     * Confirmed by Mark against the certificate, 2026-09-03. The values the
     * first version of this app carried (0.0235 V, 25.4520 ug/L/V) were close
     * but not this unit's, so anything logged before that date is off by ~3%
     * in slope and ~0.06 ug/L in offset. Recomputable from the volts_mean field
     * carried in every packet. */
    {"chlCalOffsetV", CHL_CFG_FLOAT, "V",
     "Turner cal: blank/offset voltage in pure water",
     {.f = 0.0291f}, {.f = -1.0f}, {.f = 5.0f}},

    {"chlCalScale", CHL_CFG_FLOAT, "ug/L/V",
     "Turner cal: coefficient, micrograms per litre per volt",
     {.f = 24.6872f}, {.f = 0.0001f}, {.f = 100000.0f}},
};

static const size_t kTableCount = sizeof(kTable) / sizeof(kTable[0]);

const chlCfgEntry_t *chlCfgTable(size_t &count) {
  count = kTableCount;
  return kTable;
}

/*! Map a key name to the live struct field it backs. */
static void *fieldFor(const char *key) {
  if (!strcmp(key, "chlAggPeriodMs")) return &chlCfg.aggPeriodMs;
  if (!strcmp(key, "chlFirstWindowMs")) return &chlCfg.firstWindowMs;
  if (!strcmp(key, "chlFirstWipeDelayMs")) return &chlCfg.firstWipeDelayMs;
  if (!strcmp(key, "chlSamplePeriodMs")) return &chlCfg.samplePeriodMs;
  if (!strcmp(key, "chlWipeIntervalMin")) return &chlCfg.wipeIntervalMin;
  if (!strcmp(key, "chlWipeSweeps")) return &chlCfg.wipeSweeps;
  if (!strcmp(key, "chlParkUs")) return &chlCfg.parkUs;
  if (!strcmp(key, "chlSweepUs")) return &chlCfg.sweepUs;
  if (!strcmp(key, "chlTravelMs")) return &chlCfg.travelMs;
  if (!strcmp(key, "chlSettleMs")) return &chlCfg.settleMs;
  if (!strcmp(key, "chlStallMa")) return &chlCfg.stallMa;
  if (!strcmp(key, "chlPwrAddr")) return &chlCfg.pwrAddr;
  if (!strcmp(key, "chlAdsPga")) return &chlCfg.adsPga;
  if (!strcmp(key, "chlRawLog")) return &chlCfg.rawLog;
  if (!strcmp(key, "chlWipeOnBoot")) return &chlCfg.wipeOnBoot;
  if (!strcmp(key, "chlSatCounts")) return &chlCfg.satCounts;
  if (!strcmp(key, "chlTxCellularOnly")) return &chlCfg.txCellularOnly;
  if (!strcmp(key, "chlCalOffsetV")) return &chlCfg.calOffsetV;
  if (!strcmp(key, "chlCalScale")) return &chlCfg.calScale;
  return nullptr;
}

void chlCfgLoad(void) {
  for (size_t i = 0; i < kTableCount; i++) {
    const chlCfgEntry_t &e = kTable[i];
    void *field = fieldFor(e.key);
    if (field == nullptr) {
      continue; // table and struct out of sync; caught by the boot banner
    }

    if (e.type == CHL_CFG_UINT) {
      uint32_t v = e.dflt.u;
      get_config_uint(BM_CFG_PARTITION_USER, e.key, strlen(e.key), &v);
      if (v < e.min.u || v > e.max.u) {
        printf("WARN - %s = %lu is outside %lu..%lu, using default %lu\n", e.key,
               (unsigned long)v, (unsigned long)e.min.u, (unsigned long)e.max.u,
               (unsigned long)e.dflt.u);
        v = e.dflt.u;
      }
      *(uint32_t *)field = v;
    } else {
      float v = e.dflt.f;
      get_config_float(BM_CFG_PARTITION_USER, e.key, strlen(e.key), &v);
      if (v < e.min.f || v > e.max.f) {
        printf("WARN - %s = %f is outside %f..%f, using default %f\n", e.key, (double)v,
               (double)e.min.f, (double)e.max.f, (double)e.dflt.f);
        v = e.dflt.f;
      }
      *(float *)field = v;
    }
  }
}

bool chlCfgSet(const char *key, const char *valStr, bool persist) {
  for (size_t i = 0; i < kTableCount; i++) {
    const chlCfgEntry_t &e = kTable[i];
    if (strcmp(e.key, key) != 0) {
      continue;
    }
    void *field = fieldFor(e.key);
    if (field == nullptr) {
      printf("ERR - %s has no backing field.\n", key);
      return false;
    }

    if (e.type == CHL_CFG_UINT) {
      char *end = nullptr;
      const unsigned long parsed = strtoul(valStr, &end, 0);
      if (end == valStr || *end != '\0') {
        printf("ERR - '%s' is not an integer.\n", valStr);
        return false;
      }
      if (parsed < e.min.u || parsed > e.max.u) {
        printf("ERR - %s must be %lu..%lu, got %lu.\n", key, (unsigned long)e.min.u,
               (unsigned long)e.max.u, parsed);
        return false;
      }
      *(uint32_t *)field = (uint32_t)parsed;
      if (persist) {
        set_config_uint(BM_CFG_PARTITION_USER, key, strlen(key), (uint32_t)parsed);
      }
      printf("%s = %lu %s\n", key, parsed, e.units);
    } else {
      char *end = nullptr;
      const float parsed = strtof(valStr, &end);
      if (end == valStr || *end != '\0') {
        printf("ERR - '%s' is not a number.\n", valStr);
        return false;
      }
      if (parsed < e.min.f || parsed > e.max.f) {
        printf("ERR - %s must be %f..%f, got %f.\n", key, (double)e.min.f, (double)e.max.f,
               (double)parsed);
        return false;
      }
      *(float *)field = parsed;
      if (persist) {
        set_config_float(BM_CFG_PARTITION_USER, key, strlen(key), parsed);
      }
      printf("%s = %f %s\n", key, (double)parsed, e.units);
    }

    if (persist) {
      // false = do not restart. Config keys that need a restart say so in their
      // help text; the rest are picked up live by chlCfgApply().
      save_config(BM_CFG_PARTITION_USER, false);
      printf("saved to NVM\n");
    }
    return true;
  }

  printf("ERR - unknown key '%s'. Try `chl cfg` for the list.\n", key);
  return false;
}

void chlCfgPrint(void) {
  printf("--- chl config ---\n");
  for (size_t i = 0; i < kTableCount; i++) {
    const chlCfgEntry_t &e = kTable[i];
    void *field = fieldFor(e.key);
    if (field == nullptr) {
      printf("  %-20s <NO BACKING FIELD>\n", e.key);
      continue;
    }
    if (e.type == CHL_CFG_UINT) {
      printf("  %-20s %10lu %-8s [%lu..%lu] %s\n", e.key, (unsigned long)*(uint32_t *)field,
             e.units, (unsigned long)e.min.u, (unsigned long)e.max.u, e.help);
    } else {
      printf("  %-20s %10.6f %-8s [%g..%g] %s\n", e.key, (double)*(float *)field, e.units,
             (double)e.min.f, (double)e.max.f, e.help);
    }
  }
}
