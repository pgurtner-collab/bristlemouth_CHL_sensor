/*
 * Turner Designs C-FLUOR chlorophyll sensor + antifouling wiper, on a
 * Bristlemouth Dev Kit.
 *
 * The sensor's 0-5 V analog output is read through an ADS1115 16-bit ADC on the
 * AIN0/AIN1 differential input, converted to ug/L with the unit's Turner
 * calibration, and averaged over a window (60 s by default). A goBILDA Proton
 * servo on a PCA9685 sweeps a brush across the optical window on a timer.
 *
 * Results go out three ways:
 *   - the Dev Kit's own USB console (local debugging)
 *   - Spotter's USB console and SD card (bm/<node id>/chl_agg.log)
 *   - a 46-byte binary packet over cellular/Iridium, retrievable from the Sofar
 *     API's /api/raw-messages. See CHL_PACKET_LAYOUT below.
 *
 * Readings taken while the wiper is moving, or during the settling time just
 * after, are excluded from the reported average and counted separately. The
 * brush physically crosses the optical path, so those samples are not
 * measurements of anything.
 *
 * Every tunable is a runtime config key - see chl_config.cpp. `chl` on the
 * console is the bench tool for jogging the servo and storing new endpoints.
 */

#include "user_code.h"
#include "ads1115.h"
#include "app_util.h"
#include "avgSampler.h"
#include "bristlefin.h"
#include "bsp.h"
#include "chl_app.h"
#include "chl_config.h"
#include "debug.h"
#include "ina232.h"
#include "motor_code.h"
#include "sensors.h"
#include "spotter.h"
#include "stm32_rtc.h"
#include "uptime.h"
#include <math.h>
#include <string.h>

#define LED_ON_TIME_MS 20
#define LED_PERIOD_MS 1000


/* Sample-buffer sizing. AveragingSampler stores a double per sample, so the
 * cap below is ~32 kB of heap in the worst case. The bounds on aggPeriodMs and
 * samplePeriodMs in the config table keep the ratio sane; this is the backstop. */
#define SAMPLE_MARGIN 8
#define MAX_SAMPLES_CAP 4096

/* CHL_PACKET_LAYOUT - 47 bytes, little-endian (ARM), packed, no padding.
 *
 *   off  size type     field
 *   0    1    uint8    magic         0xC1, identifies this as a CHL packet
 *   1    1    uint8    version       layout version, currently 1
 *   2    1    uint8    flags         see CHL_FLAG_* below
 *   3    1    uint8    pga           ADS1115 range index in use
 *   4    4    uint32   uptime_s      mote uptime at window close
 *   8    2    uint16   n_clean       samples in the reported average
 *   10   2    uint16   n_dirty       samples discarded (wiper moving/settling)
 *   12   4    float32  chl_mean      ug/L
 *   16   4    float32  chl_min       ug/L
 *   20   4    float32  chl_max       ug/L
 *   24   4    float32  chl_std       ug/L
 *   28   4    float32  volts_mean    raw differential volts
 *   32   2    uint16   adc_err       failed conversions this window
 *   34   2    uint16   sat_count     samples at/near the ADC rail this window
 *   36   2    uint16   wipe_count    cycles since boot
 *   38   2    uint16   wipe_base_ma  bus current before the last cycle
 *   40   2    uint16   wipe_mean_ma  mean bus current during it
 *   42   2    uint16   wipe_peak_ma  peak bus current during it
 *   44   2    uint16   wipe_dur_ms   measured duration of the last cycle
 *   46   1    uint8    checksum      sum of bytes 0..45, mod 256
 *
 * volts_mean is carried alongside chl_mean deliberately. The Turner calibration
 * is two numbers off a certificate, and if the wrong ones were loaded the whole
 * deployment's ug/L would be wrong with nothing to recover it from. Shipping the
 * volts costs 4 bytes and makes the concentration recomputable after the fact.
 *
 * The wipe fields describe the MOST RECENT cycle, repeated in every packet
 * rather than sent only in the window a wipe happened to fall in. Wipes are 30
 * minutes apart and windows are 60 seconds, so tying the telemetry to one window
 * would mean a single lost message loses the only evidence the wiper ran.
 *
 * The magic byte and trailing checksum exist because of how this arrives at the
 * other end. /api/raw-messages is an undifferentiated stream of every message
 * the buoy sent - waves, GPS, the temperature string, ours - each one an opaque
 * hex blob with a leading Sofar header byte we do not control. There is no field
 * saying "this one is from the chlorophyll node". So the decoder finds our
 * packets by scanning for 0xC1 0x01 and confirming the checksum, which is
 * reliable in a way that matching on length or a header byte is not.
 *
 * Decode with tools/decode_chl_packet.py. */
typedef struct {
  uint8_t magic;
  uint8_t version;
  uint8_t flags;
  uint8_t pga;
  uint32_t uptime_s;
  uint16_t n_clean;
  uint16_t n_dirty;
  float chl_mean;
  float chl_min;
  float chl_max;
  float chl_std;
  float volts_mean;
  uint16_t adc_err;
  uint16_t sat_count;
  uint16_t wipe_count;
  uint16_t wipe_base_ma;
  uint16_t wipe_mean_ma;
  uint16_t wipe_peak_ma;
  uint16_t wipe_dur_ms;
  uint8_t checksum;
} __attribute__((__packed__)) chlData_t;

#define CHL_PACKET_MAGIC 0xC1
#define CHL_PACKET_VERSION 1
#define CHL_DATA_SIZE sizeof(chlData_t)

#define CHL_FLAG_ADS_OK 0x01
#define CHL_FLAG_MOTOR_OK 0x02
#define CHL_FLAG_WIPE_STALLED 0x04
#define CHL_FLAG_WIPE_ACTIVE 0x08
#define CHL_FLAG_NO_CLEAN_SAMPLES 0x10
#define CHL_FLAG_SATURATED 0x20
#define CHL_FLAG_WIPE_NEVER_RAN 0x40

//pin config
//VDD to 3V3 BM rail
//GND to BM GND
//SCL to BM I2C SCL
//SDA to BM I2C SDA
//ADDR to BM GND, giving 0x48 (VDD = 0x49, SDA = 0x4A, SCL = 0x4B).
//NOTE: the board shows up on I2C mux channel 2. Do not confuse it with the 0x4A
//  device on channel 1, which is on-board hardware, not this ADC.
//A0 to CHL sensor High output
//A1 to CHL sensor Sense GND output

static ADS1115 chlSensor(&i2c1, ADS1115_ADDR_GND);

static bool adsReady = false;
static AveragingSampler chlSamples;
static uint32_t maxSamples = 0;

// Window accumulators.
static double voltsSum = 0.0;
static uint32_t dirtyCount = 0;
static uint32_t adcErrCount = 0;
static uint32_t satCount = 0;

// Set when a wiper cycle ends, so settling can be timed from it.
static uint32_t lastWipeEndMs = 0;
static bool everWiped = false;

// ---------------------------------------------------------------------------

/*! Apply the Turner calibration: ug/L = (volts - offset) * scale. */
static inline float voltsToUgl(float volts) {
  return (volts - chlCfg.calOffsetV) * chlCfg.calScale;
}

void chlAppApplyConfig(void) {
  chlSensor.setPga((ads1115_pga_e)chlCfg.adsPga);
  chlSensor.setSaturationCounts((int16_t)chlCfg.satCounts);

  motorConfig_t mc = {
      .park_us = (uint16_t)chlCfg.parkUs,
      .sweep_us = (uint16_t)chlCfg.sweepUs,
      .travel_ms = (uint16_t)chlCfg.travelMs,
      .sweeps = (uint8_t)chlCfg.wipeSweeps,
      .stall_ma = (uint16_t)chlCfg.stallMa,
  };
  motorSetConfig(mc);
  motorSetPowerMonitorAddress((uint8_t)chlCfg.pwrAddr);
}

chlReading_t chlAppReadOnce(void) {
  chlReading_t r = {};
  float volts = 0.0f;
  int16_t counts = 0;
  r.ok = chlSensor.readVoltage(volts, &counts);
  if (r.ok) {
    r.counts = counts;
    r.volts = volts;
    r.ugl = voltsToUgl(volts);
    r.saturated = chlSensor.lastReadSaturated();
  }
  return r;
}

/*! True while the wiper is moving or still settling afterwards. */
static bool wiperDisturbing(void) {
  if (motorIsBusy()) {
    return true;
  }
  if (!everWiped) {
    return false;
  }
  return ((uint32_t)uptimeGetMs() - lastWipeEndMs) < chlCfg.settleMs;
}

/*! Close out the averaging window: compute, report, transmit, reset. */
void chlAppReportNow(void) {
  double mean = 0, stdev = 0, minv = 0, maxv = 0;
  const uint32_t n = chlSamples.getNumSamples();

  if (n) {
    mean = chlSamples.getMean();
    stdev = chlSamples.getStd(mean);
    minv = chlSamples.getMin();
    maxv = chlSamples.getMax();
  }
  const double voltsMean = n ? (voltsSum / (double)n) : 0.0;
  chlSamples.clear();

  RTCTimeAndDate_t time_and_date = {};
  rtcGet(&time_and_date);
  char rtcTimeBuffer[32] = {};
  rtcPrint(rtcTimeBuffer, &time_and_date);

  const uint32_t uptime_s = (uint32_t)(uptimeGetMs() / 1000);
  const motorWipeStats_t &w = motorGetLastWipe();

  uint8_t flags = 0;
  if (adsReady) flags |= CHL_FLAG_ADS_OK;
  if (motorIsReady()) flags |= CHL_FLAG_MOTOR_OK;
  if (w.valid && w.stalled) flags |= CHL_FLAG_WIPE_STALLED;
  if (motorIsBusy()) flags |= CHL_FLAG_WIPE_ACTIVE;
  if (n == 0) flags |= CHL_FLAG_NO_CLEAN_SAMPLES;
  if (satCount) flags |= CHL_FLAG_SATURATED;
  if (!w.valid) flags |= CHL_FLAG_WIPE_NEVER_RAN;

  /* A window with no clean samples is either a dead sensor or a wiper that never
   * stopped moving. Both are worth shouting about rather than quietly
   * transmitting zeros that will plot as a real measurement. */
  if (n == 0) {
    printf("ERR - CHL window closed with 0 clean samples (%lu dirty, %lu adc errors). "
           "Check the ADS1115 and the wiper schedule.\n",
           (unsigned long)dirtyCount, (unsigned long)adcErrCount);
    spotter_log_console(0, "[chl] ERR window with 0 clean samples");
  }

  printf("[chl-agg] | uptime: %lus, rtc: %s, n: %lu, dirty: %lu, mean: %.4f ug/L, min: %.4f, "
         "max: %.4f, std: %.4f, volts: %.6f, adc_err: %lu, sat: %lu, wipes: %u, "
         "last_wipe[peak: %u mA, mean: %u mA, base: %u mA, %u ms]%s\n",
         (unsigned long)uptime_s, rtcTimeBuffer, (unsigned long)n, (unsigned long)dirtyCount,
         mean, minv, maxv, stdev, voltsMean, (unsigned long)adcErrCount,
         (unsigned long)satCount, w.count, w.peak_ma, w.mean_ma, w.baseline_ma, w.duration_ms,
         (w.valid && w.stalled) ? "  *** WIPER STALL ***" : "");

  BmErr log_err = spotter_log(0, "chl_agg.log", USE_TIMESTAMP,
                              "uptime: %lus, rtc: %s, n: %lu, dirty: %lu, mean_ugl: %.4f, "
                              "min_ugl: %.4f, max_ugl: %.4f, std_ugl: %.4f, volts: %.6f, "
                              "adc_err: %lu, sat: %lu, wipe_count: %u, wipe_peak_ma: %u, "
                              "wipe_mean_ma: %u, wipe_base_ma: %u, wipe_ms: %u, stall: %u\n",
                              (unsigned long)uptime_s, rtcTimeBuffer, (unsigned long)n,
                              (unsigned long)dirtyCount, mean, minv, maxv, stdev, voltsMean,
                              (unsigned long)adcErrCount, (unsigned long)satCount, w.count,
                              w.peak_ma, w.mean_ma, w.baseline_ma, w.duration_ms,
                              (unsigned)(w.valid && w.stalled));

  spotter_log_console(0, "[chl-agg] | n: %lu, mean: %.4f ug/L, std: %.4f, wipes: %u, peak: %u mA",
                      (unsigned long)n, mean, stdev, w.count, w.peak_ma);

  if (log_err != BmOK) {
    printf("[chl-agg] | WARN Spotter log publish failed, BmErr=%d%s\n", log_err,
           log_err == BmENETDOWN ? " (network down - is the Spotter connected and on?)" : "");
  }

  chlData_t tx = {
      .magic = CHL_PACKET_MAGIC,
      .version = CHL_PACKET_VERSION,
      .flags = flags,
      .pga = (uint8_t)chlSensor.getPga(),
      .uptime_s = uptime_s,
      .n_clean = (uint16_t)((n > UINT16_MAX) ? UINT16_MAX : n),
      .n_dirty = (uint16_t)((dirtyCount > UINT16_MAX) ? UINT16_MAX : dirtyCount),
      .chl_mean = (float)mean,
      .chl_min = (float)minv,
      .chl_max = (float)maxv,
      .chl_std = (float)stdev,
      .volts_mean = (float)voltsMean,
      .adc_err = (uint16_t)((adcErrCount > UINT16_MAX) ? UINT16_MAX : adcErrCount),
      .sat_count = (uint16_t)((satCount > UINT16_MAX) ? UINT16_MAX : satCount),
      .wipe_count = w.count,
      .wipe_base_ma = w.baseline_ma,
      .wipe_mean_ma = w.mean_ma,
      .wipe_peak_ma = w.peak_ma,
      .wipe_dur_ms = w.duration_ms,
      .checksum = 0, // filled in below, once the rest of the bytes are known
  };

  uint8_t tx_data[CHL_DATA_SIZE] = {};
  memcpy(tx_data, (uint8_t *)(&tx), CHL_DATA_SIZE);

  // Checksum every byte ahead of it, then write it into the outgoing buffer.
  uint8_t sum = 0;
  for (size_t i = 0; i < CHL_DATA_SIZE - 1; i++) {
    sum = (uint8_t)(sum + tx_data[i]);
  }
  tx_data[CHL_DATA_SIZE - 1] = sum;

  /* NOTE: spotter_tx_data() returns BmErr, and BmOK is 0 - so a bare
   * `if (spotter_tx_data(...))` is true on FAILURE. The upstream
   * rbr_coda_example gets this backwards and prints "Sucessfully sent" whenever
   * the call actually failed. Compare against BmOK explicitly. */
  const BmSerialNetworkType net = chlCfg.txCellularOnly ? BmNetworkTypeCellularOnly
                                                        : BmNetworkTypeCellularIriFallback;
  /* BmOK here means only that the publish left this node. It says nothing about
   * whether the Spotter accepted it, queued it, or transmitted it - that is
   * decided Spotter-side and reported only on the Spotter's own console. Do not
   * word this as if it were a delivery confirmation: on 2026-09-03 the mote
   * printed success for packets the Spotter was rejecting outright with
   * "Queue MS_Q_CELLULAR_ONLY is full", and the wording is what made that take
   * two consoles to notice. */
  BmErr tx_err = spotter_tx_data(tx_data, CHL_DATA_SIZE, net);
  if (tx_err == BmOK) {
    printf("[chl-agg] | published %u bytes to the Spotter (%s). NOT a delivery "
           "confirmation - check the Spotter console for \"Submitted "
           "spotter/transmit-data\".\n",
           (unsigned)CHL_DATA_SIZE, chlCfg.txCellularOnly ? "cellular only" : "cell+iridium");
  } else {
    printf("[chl-agg] | ERR publish failed before it even reached the Spotter, BmErr=%d\n",
           tx_err);
  }

  voltsSum = 0.0;
  dirtyCount = 0;
  adcErrCount = 0;
  satCount = 0;
}

void chlAppStatus(void) {
  const motorWipeStats_t &w = motorGetLastWipe();
  const motorConfig_t &mc = motorGetConfig();

  printf("--- chl status ---\n");
  printf("  ADS1115   : %s at 0x%02X, range +-%.3f V, PGA idx %u\n",
         adsReady ? "ready" : "NOT INITIALIZED", (unsigned)chlSensor.getAddress(),
         (double)chlSensor.getFullScaleVolts(), (unsigned)chlSensor.getPga());
  printf("  calibration: ug/L = (V - %.6f) * %.4f   (clip flag at %ld counts = %.3f V "
         "= %.1f ug/L)\n",
         (double)chlCfg.calOffsetV, (double)chlCfg.calScale, (long)chlCfg.satCounts,
         (double)((float)chlCfg.satCounts * chlSensor.getLsbVolts()),
         (double)(((float)chlCfg.satCounts * chlSensor.getLsbVolts() - chlCfg.calOffsetV) *
                  chlCfg.calScale));
  printf("  transmit  : %s\n",
         chlCfg.txCellularOnly ? "cellular only" : "cellular with Iridium fallback");

  chlReading_t r = chlAppReadOnce();
  if (r.ok) {
    printf("  reading   : %d counts, %.6f V, %.4f ug/L%s\n", r.counts, (double)r.volts,
           (double)r.ugl, r.saturated ? "   *** AT ADC RAIL, reading is clipped ***" : "");
  } else {
    printf("  reading   : FAILED\n");
  }

  printf("  window    : first %lu ms then %lu ms, sampling every %lu ms, %lu clean samples "
         "buffered so far\n",
         (unsigned long)chlCfg.firstWindowMs, (unsigned long)chlCfg.aggPeriodMs,
         (unsigned long)chlCfg.samplePeriodMs, (unsigned long)chlSamples.getNumSamples());
  printf("  uptime    : %lus  (if this keeps resetting, the Spotter's bridge power "
         "controller is duty-cycling the bus)\n",
         (unsigned long)(uptimeGetMs() / 1000));
  printf("  wiper     : %s, %s, pulse now %u us\n", motorIsReady() ? "ready" : "NOT INITIALIZED",
         motorIsBusy() ? "MOVING" : "idle", motorGetPulseUs());
  printf("  endpoints : park %u us, sweep %u us, travel %u ms, %u sweeps, stall > %u mA\n",
         mc.park_us, mc.sweep_us, mc.travel_ms, mc.sweeps, mc.stall_ma);
  /* Report what will actually happen, not what one key says in isolation. The
   * boot wipe and the free-running interval are independent, so "interval = 0"
   * alone means nothing - with chlWipeOnBoot set it is the normal duty-cycled
   * configuration, and only both being off is genuinely no wiping. */
  if (chlCfg.wipeOnBoot && chlCfg.wipeIntervalMin) {
    printf("  schedule  : once per power-on, plus every %lu min while powered\n",
           (unsigned long)chlCfg.wipeIntervalMin);
  } else if (chlCfg.wipeOnBoot) {
    printf("  schedule  : once per power-on (the bus interval sets the wipe interval)\n");
  } else if (chlCfg.wipeIntervalMin) {
    printf("  schedule  : every %lu min while powered, none on boot\n",
           (unsigned long)chlCfg.wipeIntervalMin);
  } else {
    printf("  schedule  : NO WIPING (chlWipeOnBoot = 0 and chlWipeIntervalMin = 0)\n");
  }
  if (w.valid) {
    printf("  last wipe : #%u at uptime %lus, %u ms, base %u mA, mean %u mA, peak %u mA, "
           "n %u%s\n",
           w.count, (unsigned long)w.end_uptime_s, w.duration_ms, w.baseline_ma, w.mean_ma,
           w.peak_ma, w.samples, w.stalled ? "  *** STALLED ***" : "");
  } else {
    printf("  last wipe : none yet\n");
  }
  printf("  current   : read from INA232 at 0x%02X\n", (unsigned)chlCfg.pwrAddr);
  printf("  packet    : %u bytes, magic 0x%02X, version %u\n", (unsigned)CHL_DATA_SIZE,
         (unsigned)CHL_PACKET_MAGIC, (unsigned)CHL_PACKET_VERSION);
}

void chlAppScanI2C(void) {
  struct {
    uint8_t addr;
    const char *what;
  } expected[] = {
      {0x40, "PCA9685 servo driver"},
      {0x48, "ADS1115 ADC"},
      {0x41, "INA232 power monitor (PoDL)"},
      {0x43, "INA232 power monitor (main)"},
      {0x70, "TCA9546A I2C mux"},
      {0x76, "BME280"},
  };

  printf("--- i2c scan ---\n");
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
    uint8_t probe = 0;
    const I2CResponse_t rc =
        i2cProbe(&i2c1, expected[i].addr, 100);
    (void)probe;
    printf("  0x%02X  %-32s %s\n", (unsigned)expected[i].addr, expected[i].what,
           (rc == I2C_OK) ? "present" : "NO RESPONSE");
  }
}

// ---------------------------------------------------------------------------

void setup(void) {
  chlCfgLoad();

  /* Clear the PCA9685's ALL-CALL address before anything else on this bus.
   * Out of reset it also answers to 0x70, which is the I2C mux. sensorsInit()
   * has unfortunately already talked to the mux by the time user setup() runs,
   * so this closes the window as early as firmware can - see motorClearAllCall()
   * for why the real fix is a wiring change. */
  motorClearAllCall();

  // Enable the input to the Vout power supply. Without this, Vout and 5V stay off.
  bristlefin.enableVbus();
  // ensure Vbus stable before enable Vout with a 5ms delay.
  vTaskDelay(pdMS_TO_TICKS(5));
  // enable Vout, 12V by default.
  bristlefin.enableVout();
  // enable 5V out - powers the servo through the PCA9685's V+ terminal.
  bristlefin.enable5V();
  bristlefin.enable3V();

  // Let the ADS1115's supply come up before talking to it.
  vTaskDelay(pdMS_TO_TICKS(50));

  chlSensor.setPga((ads1115_pga_e)chlCfg.adsPga);
  chlSensor.setSaturationCounts((int16_t)chlCfg.satCounts);
  adsReady = chlSensor.init();
  if (!adsReady) {
    printf("ERROR - Failed to initialize ADS1115 chlorophyll sensor!\n");
  }

  motorConfig_t mc = {
      .park_us = (uint16_t)chlCfg.parkUs,
      .sweep_us = (uint16_t)chlCfg.sweepUs,
      .travel_ms = (uint16_t)chlCfg.travelMs,
      .sweeps = (uint8_t)chlCfg.wipeSweeps,
      .stall_ma = (uint16_t)chlCfg.stallMa,
  };
  motorInit(mc);
  motorSetPowerMonitorAddress((uint8_t)chlCfg.pwrAddr);

  /* Speed up the shared INA232s.
   *
   * powerInit() leaves them at 256x averaging with 1.1 ms conversions, which is
   * 563 ms per reading. A wiper sweep is under a second, so at that setting the
   * current peak - the whole point of measuring - is averaged into nothing. 16x
   * at 140 us gives ~4.5 ms per reading instead.
   *
   * Side effect, and it is a real one: the board's own `power |` log lines get
   * noisier, because they now report 16-sample averages rather than 256-sample
   * ones. The quantisation floor is unchanged (250 uA per LSB either way). */
  {
    using namespace INA;
    INA232 fastIna[2] = {INA232(&i2c1, I2C_INA_MAIN_ADDR), INA232(&i2c1, I2C_INA_PODL_ADDR)};
    for (size_t i = 0; i < 2; i++) {
      fastIna[i].setAvg(AVG_16);
      fastIna[i].setBusConvTime(CT_140);
      fastIna[i].setShuntConvTime(CT_140);
    }
  }

  /* Size the clean-sample buffer for whichever window is longer. Normally that
   * is aggPeriodMs, but firstWindowMs can legitimately be set larger on a bus
   * that is left powered continuously. */
  const uint32_t longestWindowMs =
      (chlCfg.firstWindowMs > chlCfg.aggPeriodMs) ? chlCfg.firstWindowMs : chlCfg.aggPeriodMs;
  maxSamples = (longestWindowMs / chlCfg.samplePeriodMs) + SAMPLE_MARGIN;
  if (maxSamples > MAX_SAMPLES_CAP) {
    printf("WARN - window/sample period needs %lu samples, capping at %d. Readings past the "
           "cap will be dropped from each window.\n",
           (unsigned long)maxSamples, MAX_SAMPLES_CAP);
    maxSamples = MAX_SAMPLES_CAP;
  }
  chlSamples.initBuffer(maxSamples);

  chlCliInit();

  printf("\nCHL wiper node ready.\n");
  chlCfgPrint();
  printf("Type `chl` for the bench commands (jog the servo, set endpoints, force a "
         "transmit).\n\n");
}

void loop(void) {
  // Advance any in-progress wiper movement and sample the wipe current. Non-
  // blocking, so the chlorophyll sampling below keeps running while it moves.
  const bool wasBusy = motorIsBusy();
  motorService();
  if (wasBusy && !motorIsBusy()) {
    lastWipeEndMs = (uint32_t)uptimeGetMs();
    everWiped = true;
  }

  /* Wipe once shortly after every power-on, independently of the free-running
   * interval below.
   *
   * These have to be separate. On a duty-cycled Bristlemouth bus the node's
   * uptime resets every power window, so a free-running interval longer than the
   * window never fires and the boot wipe IS the whole schedule. Gating the boot
   * wipe on chlWipeIntervalMin > 0 - as this did until 2026-09-03 - meant that
   * setting the interval to 0 to disable the useless timer silently disabled all
   * wiping, and the first packet after the change reported wipes=0. */
  static bool bootWipeDone = false;
  static uint32_t motorCleanTimer = 0;
  if (!bootWipeDone && (uint32_t)uptimeGetMs() >= chlCfg.firstWipeDelayMs) {
    bootWipeDone = true;
    motorCleanTimer = (uint32_t)uptimeGetMs();
    if (chlCfg.wipeOnBoot) {
      motorStartCleaningCycle((uint8_t)chlCfg.wipeSweeps);
    }
  }

  // Additional cycles on a timer, for a bus that is left powered continuously.
  // Useless when the bus is duty-cycled, hence 0 being a normal setting.
  if (chlCfg.wipeIntervalMin > 0 && bootWipeDone) {
    const uint32_t intervalMs = chlCfg.wipeIntervalMin * 60000UL;
    if ((uint32_t)uptimeGetMs() - motorCleanTimer >= intervalMs) {
      motorCleanTimer += intervalMs;
      motorStartCleaningCycle((uint8_t)chlCfg.wipeSweeps);
    }
  }

  // Read the chlorophyll sensor on its own cadence.
  static uint32_t chlReadTimer = uptimeGetMs();
  if ((uint32_t)uptimeGetMs() - chlReadTimer >= chlCfg.samplePeriodMs) {
    chlReadTimer += chlCfg.samplePeriodMs;

    /* The wiper physically obstructs the sensor while it moves, so check both
     * before and after the conversion - a cycle can start or finish while one is
     * in flight, and a reading that straddles the boundary is not trustworthy
     * either way. */
    const bool disturbedBefore = wiperDisturbing();

    float volts = 0.0f;
    int16_t counts = 0;
    if (chlSensor.readVoltage(volts, &counts)) {
      const bool disturbed = disturbedBefore || wiperDisturbing();
      const float ugl = voltsToUgl(volts);

      if (chlSensor.lastReadSaturated()) {
        satCount++;
      }

      if (disturbed) {
        dirtyCount++;
      } else if (chlSamples.getNumSamples() < maxSamples) {
        chlSamples.addSample((double)ugl);
        voltsSum += (double)volts;
      }

      if (chlCfg.rawLog) {
        RTCTimeAndDate_t t = {};
        rtcGet(&t);
        char rtcBuf[32];
        rtcPrint(rtcBuf, &t);
        spotter_log(0, "chl_raw.log", USE_TIMESTAMP,
                    "tick: %llu, rtc: %s, counts: %d, volts: %.6f, chl_ugl: %.6f, dirty: %u\n",
                    uptimeGetMs(), rtcBuf, counts, (double)volts, (double)ugl,
                    (unsigned)disturbed);
      }
    } else {
      adcErrCount++;
      printf("ERROR - Failed to read ADS1115 chlorophyll sensor!\n");
    }
  }

  /* Close the averaging window on schedule. The first window after boot is
   * short - see chlFirstWindowMs - so that a node on a duty-cycled Bristlemouth
   * bus gets a packet out before the rail drops. */
  static uint32_t aggTimer = uptimeGetMs();
  static bool firstWindowDone = false;
  const uint32_t windowMs = firstWindowDone ? chlCfg.aggPeriodMs : chlCfg.firstWindowMs;
  if ((uint32_t)uptimeGetMs() - aggTimer >= windowMs) {
    aggTimer += windowMs; // advance by the period so windows don't drift
    firstWindowDone = true;
    chlAppReportNow();
  }

  // Heartbeat LED1, blinks green every LED_PERIOD_MS milliseconds.
  static bool ledState = false;
  static uint64_t ledLastScheduledOnTime = uptimeGetMs();
  const uint64_t elapsedSinceOnTime = uptimeGetMs() - ledLastScheduledOnTime;
  if (!ledState && elapsedSinceOnTime >= LED_PERIOD_MS) {
    ledLastScheduledOnTime += LED_PERIOD_MS;
    bristlefin.setLed(1, Bristlefin::LED_GREEN);
    ledState = true;
  } else if (ledState && elapsedSinceOnTime >= LED_ON_TIME_MS) {
    bristlefin.setLed(1, Bristlefin::LED_OFF);
    ledState = false;
  }
}
