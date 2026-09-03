#include "motor_code.h"
#include "FreeRTOS.h"
#include "abstract/abstract_i2c.h"
#include "bsp.h"
#include "debug.h"
#include "task.h"
#include "uptime.h"

// ---------------------------------------------------------------------------
// PCA9685 register map
// ---------------------------------------------------------------------------
#define PCA9685_REG_MODE1 0x00
#define PCA9685_REG_MODE2 0x01
#define PCA9685_REG_ALLCALLADR 0x05
#define PCA9685_REG_LED0_ON_L 0x06
#define PCA9685_REG_PRESCALE 0xFE

// MODE1 bits
#define PCA9685_MODE1_RESTART 0x80
#define PCA9685_MODE1_AI 0x20     // register address auto-increment
#define PCA9685_MODE1_SLEEP 0x10  // low-power, oscillator off
#define PCA9685_MODE1_ALLCALL 0x01

// MODE2 bits
#define PCA9685_MODE2_OUTDRV 0x04 // totem-pole outputs, what a servo input wants

// Each channel occupies 4 registers: ON_L, ON_H, OFF_L, OFF_H
#define PCA9685_REG_PER_CHANNEL 4
#define PCA9685_CHANNEL_MAX 15

// The counter is 12-bit, so one PWM period is 4096 ticks.
#define PCA9685_TICKS_PER_PERIOD 4096

// Bit 4 of OFF_H forces the output fully off regardless of the count.
#define PCA9685_FULL_OFF_BIT 0x10

// Internal oscillator, used for the prescale calculation.
#define PCA9685_OSC_HZ 25000000UL

// The oscillator needs 500us to stabilise after clearing SLEEP.
#define PCA9685_OSC_STARTUP_MS 1

/*!
  Minimal PCA9685 driver, only as much as is needed to drive a servo.
*/
class PCA9685 : public AbstractI2C {
public:
  PCA9685(I2CInterface_t *interface, uint8_t address) {
    _interface = interface;
    _addr = address;
  }

  bool clearAllCall();
  bool init();
  bool setPwmFrequency(uint16_t freq_hz);
  bool setChannelPulseUs(uint8_t channel, uint16_t pulse_us);
  bool setChannelOff(uint8_t channel);

private:
  bool writeReg(uint8_t reg, uint8_t value);
  bool readReg(uint8_t reg, uint8_t &value);

  // Actual frequency in use, needed to convert microseconds to counts.
  uint16_t freq_hz_ = MOTOR_PWM_FREQ_HZ;
};

/*!
 Drop the PCA9685's ALL-CALL address.

 IMPORTANT. Out of reset the PCA9685 answers to 0x70 in addition to its own
 address, and 0x70 is the TCA9546A I2C mux on this board. Worse, Bristlefin's
 TCA9546A::setChannel() reads the mux back to verify every channel change, so
 two devices would drive the bus at once.

 This only protects the bus from the moment it runs, and MODE1 resets to
 ALLCALL-enabled on every power-up. sensorsInit() - which brings up the mux -
 runs before user setup(), so there is a window at boot that firmware cannot
 close. The way to actually close it is in hardware: take the PCA9685's VCC from
 the switched 5V rail rather than the always-on 3V3, so the part is unpowered
 until enable5V() runs. See docs/hardware in the project repo.

 \return true if successful, false otherwise
*/
bool PCA9685::clearAllCall() {
  // Sleep is required before the prescaler can be changed later. Write ALLCALL=0
  // in the same operation so the 0x70 conflict is cleared as early as possible.
  if (!writeReg(PCA9685_REG_MODE1, PCA9685_MODE1_SLEEP | PCA9685_MODE1_AI)) {
    printf("PCA9685 failed to enter sleep / clear ALLCALL.\n");
    return false;
  }

  // Confirm the part is really there and really took the write, rather than
  // trusting a bare ACK.
  uint8_t mode1 = 0;
  if (!readReg(PCA9685_REG_MODE1, mode1)) {
    printf("PCA9685 failed to read back MODE1.\n");
    return false;
  }
  if (mode1 & PCA9685_MODE1_ALLCALL) {
    printf("PCA9685 ALLCALL still enabled (MODE1 0x%02X) - it will fight the mux at 0x70.\n",
           mode1);
    return false;
  }
  return true;
}

/*!
 Configure the PCA9685 for servo use. Assumes clearAllCall() has already run.

 \return true if successful, false otherwise
*/
bool PCA9685::init() {
  printf("PCA9685 init\n");

  if (!clearAllCall()) {
    return false;
  }

  if (!setPwmFrequency(MOTOR_PWM_FREQ_HZ)) {
    return false;
  }

  // Totem-pole outputs. Servo signal inputs are not open-drain.
  if (!writeReg(PCA9685_REG_MODE2, PCA9685_MODE2_OUTDRV)) {
    printf("PCA9685 failed to set MODE2.\n");
    return false;
  }

  printf("PCA9685 initialized at 0x%02X.\n", (uint8_t)_addr);
  return true;
}

/*!
 Set the PWM refresh rate and wake the oscillator.

 prescale = round(osc / (4096 * freq)) - 1, per the datasheet.

 \param[in] freq_hz Desired refresh rate
 \return true if successful, false otherwise
*/
bool PCA9685::setPwmFrequency(uint16_t freq_hz) {
  if (freq_hz == 0) {
    return false;
  }

  const uint32_t ticks_per_sec = (uint32_t)PCA9685_TICKS_PER_PERIOD * freq_hz;
  // Rounded integer divide, then the datasheet's -1.
  const uint32_t prescale = ((PCA9685_OSC_HZ + (ticks_per_sec / 2)) / ticks_per_sec) - 1;

  // Must already be asleep for this write to take effect.
  if (!writeReg(PCA9685_REG_PRESCALE, (uint8_t)prescale)) {
    printf("PCA9685 failed to set prescale.\n");
    return false;
  }

  // Wake up, keeping auto-increment on and ALLCALL off.
  if (!writeReg(PCA9685_REG_MODE1, PCA9685_MODE1_AI)) {
    printf("PCA9685 failed to wake.\n");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(PCA9685_OSC_STARTUP_MS));

  // RESTART re-enables any outputs that were running before the sleep.
  if (!writeReg(PCA9685_REG_MODE1, PCA9685_MODE1_AI | PCA9685_MODE1_RESTART)) {
    printf("PCA9685 failed to restart.\n");
    return false;
  }

  freq_hz_ = freq_hz;
  return true;
}

/*!
 Drive one channel with a pulse of the given width.

 The output is set to rise at count 0 and fall at the count corresponding to the
 requested width, so the pulse always starts at the beginning of the period.

 \param[in] channel  PCA9685 output, 0-15
 \param[in] pulse_us Pulse width in microseconds
 \return true if successful, false otherwise
*/
bool PCA9685::setChannelPulseUs(uint8_t channel, uint16_t pulse_us) {
  if (channel > PCA9685_CHANNEL_MAX) {
    return false;
  }

  // counts = pulse_us * ticks_per_period * freq / 1e6. Ordered to keep the
  // intermediate inside 32 bits: the worst case here is 2400 * 4096 * 50 = 4.9e8.
  uint32_t counts =
      ((uint32_t)pulse_us * PCA9685_TICKS_PER_PERIOD * freq_hz_ + 500000UL) / 1000000UL;
  if (counts > (PCA9685_TICKS_PER_PERIOD - 1)) {
    counts = PCA9685_TICKS_PER_PERIOD - 1;
  }

  // ON_L, ON_H, OFF_L, OFF_H written in one auto-incrementing burst.
  uint8_t bytes[] = {
      (uint8_t)(PCA9685_REG_LED0_ON_L + (PCA9685_REG_PER_CHANNEL * channel)),
      0x00,
      0x00,
      (uint8_t)(counts & 0xFF),
      (uint8_t)((counts >> 8) & 0x0F),
  };
  return (writeBytes(bytes, sizeof(bytes), 100) == I2C_OK);
}

/*!
 Stop driving a channel. The output is held low and the servo is left unpowered,
 so it stops holding position.

 \param[in] channel PCA9685 output, 0-15
 \return true if successful, false otherwise
*/
bool PCA9685::setChannelOff(uint8_t channel) {
  if (channel > PCA9685_CHANNEL_MAX) {
    return false;
  }

  uint8_t bytes[] = {
      (uint8_t)(PCA9685_REG_LED0_ON_L + (PCA9685_REG_PER_CHANNEL * channel)),
      0x00,
      0x00,
      0x00,
      PCA9685_FULL_OFF_BIT,
  };
  return (writeBytes(bytes, sizeof(bytes), 100) == I2C_OK);
}

bool PCA9685::writeReg(uint8_t reg, uint8_t value) {
  uint8_t bytes[] = {reg, value};
  return (writeBytes(bytes, sizeof(bytes), 100) == I2C_OK);
}

bool PCA9685::readReg(uint8_t reg, uint8_t &value) {
  if (writeBytes(&reg, sizeof(reg), 100) != I2C_OK) {
    return false;
  }
  return (readBytes(&value, sizeof(value), 100) == I2C_OK);
}

// ---------------------------------------------------------------------------
// Bus current monitor
// ---------------------------------------------------------------------------

/*!
 Direct reader for the INA232's shunt-voltage register.

 The board already has an INA232 driver and the sensorSampler task already polls
 it, but neither is usable here. That path waits on the conversion-ready flag
 with a 50 ms poll period and is configured for 256x averaging, so one reading
 represents 563 ms - longer than a whole sweep. A wiper cycle would be smeared
 into a sample or two and the current peak, which is the entire point, would be
 averaged away.

 So this reads the shunt register directly and skips the ready-flag handshake.
 In continuous-conversion mode the register always holds the last completed
 conversion, and reading it does not disturb the sensorSampler task's own polling
 (reading MASK_EN is what clears the ready flag, and this never touches it).
*/
#define INA232_REG_SHUNT_V 0x01
#define INA232_SHUNT_V_PER_LSB 2.5e-6f // volts per count
#define BRISTLEFIN_SHUNT_OHMS 0.01f

class BusMonitor : public AbstractI2C {
public:
  BusMonitor(I2CInterface_t *interface, uint8_t address) {
    _interface = interface;
    _addr = address;
  }

  // AbstractI2C declares init() pure virtual. There is nothing to initialize
  // here: this class only reads a register on a device the sensorSampler task
  // has already brought up.
  bool init() override { return true; }

  void setAddress(uint8_t address) { _addr = address; }

  /*! \param[out] milliamps Bus current. \return true on a good I2C read. */
  bool readCurrentMa(int32_t &milliamps) {
    uint8_t reg = INA232_REG_SHUNT_V;
    if (writeBytes(&reg, sizeof(reg), 20) != I2C_OK) {
      return false;
    }
    uint16_t raw = 0;
    if (readBytes((uint8_t *)&raw, sizeof(raw), 20) != I2C_OK) {
      return false;
    }
    const int16_t counts = (int16_t)__builtin_bswap16(raw);
    const float amps = ((float)counts * INA232_SHUNT_V_PER_LSB) / BRISTLEFIN_SHUNT_OHMS;
    milliamps = (int32_t)(amps * 1000.0f);
    return true;
  }
};

static BusMonitor busMonitor(&i2c1, I2C_INA_MAIN_ADDR);
static uint8_t busMonitorAddr = I2C_INA_MAIN_ADDR;

void motorSetPowerMonitorAddress(uint8_t address) {
  busMonitor.setAddress(address);
  busMonitorAddr = address;
}

/* Read the bus current as a MAGNITUDE.
 *
 * Which way current reads through the shunt depends on which INA232 is selected
 * and how the rail is fed. Measured on this board with the Dev Kit powered from
 * the Spotter's Bristlemouth bus: 0x43 reads +22 mA idle and +54 mA during a
 * wipe, while 0x41 sits at a flat -6 mA and does not respond to the servo at
 * all. On the bench supply it was the other way round. The sign is a fact about
 * wiring, not about the servo - and letting a negative value reach the uint16
 * packet fields reported a wipe mean of 65531 mA. */
static bool readBusMagnitudeMa(uint16_t &milliamps) {
  int32_t ma = 0;
  if (!busMonitor.readCurrentMa(ma)) {
    return false;
  }
  if (ma < 0) {
    ma = -ma;
  }
  milliamps = (ma > UINT16_MAX) ? UINT16_MAX : (uint16_t)ma;
  return true;
}

// ---------------------------------------------------------------------------
// Cleaning motor
// ---------------------------------------------------------------------------

static PCA9685 servoController(&i2c1, PCA9685_ADDR_DEFAULT);
static bool motorReady = false;
static uint16_t motorLastPulseUs = MOTOR_DEFAULT_PARK_US;

static motorConfig_t motorCfg = {
    .park_us = MOTOR_DEFAULT_PARK_US,
    .sweep_us = MOTOR_DEFAULT_SWEEP_US,
    .travel_ms = MOTOR_DEFAULT_TRAVEL_MS,
    .sweeps = MOTOR_DEFAULT_SWEEPS,
    .stall_ma = 600,
};

static motorWipeStats_t lastWipe = {};

bool motorIsReady(void) { return motorReady; }
uint16_t motorGetPulseUs(void) { return motorLastPulseUs; }
const motorConfig_t &motorGetConfig(void) { return motorCfg; }
const motorWipeStats_t &motorGetLastWipe(void) { return lastWipe; }

bool motorClearAllCall(void) { return servoController.clearAllCall(); }

bool motorSetConfig(const motorConfig_t &cfg) {
  if (cfg.park_us < MOTOR_MIN_PULSE_US || cfg.park_us > MOTOR_MAX_PULSE_US ||
      cfg.sweep_us < MOTOR_MIN_PULSE_US || cfg.sweep_us > MOTOR_MAX_PULSE_US) {
    printf("[motor] Rejecting endpoints outside %u-%u us.\n", (unsigned)MOTOR_MIN_PULSE_US,
           (unsigned)MOTOR_MAX_PULSE_US);
    return false;
  }
  if (cfg.travel_ms == 0 || cfg.sweeps == 0) {
    printf("[motor] Rejecting zero travel time or sweep count.\n");
    return false;
  }
  const uint16_t count = lastWipe.count; // survives a reconfiguration
  motorCfg = cfg;
  lastWipe.count = count;
  return true;
}

bool motorInit(const motorConfig_t &cfg) {
  motorSetConfig(cfg);

  motorReady = servoController.init();
  if (!motorReady) {
    printf("ERROR - Failed to initialize PCA9685 servo controller!\n");
    return false;
  }

  // Park the wiper so it starts from a known position.
  if (!motorSetPulseUs(motorCfg.park_us)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(motorCfg.travel_ms));
  return motorRelease();
}

bool motorSetPulseUs(uint16_t pulse_us) {
  if (!motorReady) {
    return false;
  }

  // Clamp rather than reject: driving a servo past its endpoints stalls it, and a
  // stalled Proton pulls up to 1 A.
  if (pulse_us < MOTOR_MIN_PULSE_US) {
    pulse_us = MOTOR_MIN_PULSE_US;
  } else if (pulse_us > MOTOR_MAX_PULSE_US) {
    pulse_us = MOTOR_MAX_PULSE_US;
  }

  if (!servoController.setChannelPulseUs(MOTOR_SERVO_CHANNEL, pulse_us)) {
    return false;
  }
  motorLastPulseUs = pulse_us;
  return true;
}

bool motorSetAngle(float degrees) {
  if (degrees < 0.0f) {
    degrees = 0.0f;
  } else if (degrees > MOTOR_RANGE_DEG) {
    degrees = MOTOR_RANGE_DEG;
  }

  const float span_us = (float)(MOTOR_MAX_PULSE_US - MOTOR_MIN_PULSE_US);
  const uint16_t pulse_us =
      (uint16_t)((float)MOTOR_MIN_PULSE_US + ((degrees / MOTOR_RANGE_DEG) * span_us) + 0.5f);

  return motorSetPulseUs(pulse_us);
}

bool motorRelease(void) {
  if (!motorReady) {
    return false;
  }
  return servoController.setChannelOff(MOTOR_SERVO_CHANNEL);
}

// Cleaning cycle state machine. Deliberately non-blocking: blocking here would
// stop loop() entirely, and the chlorophyll sampling has to keep running while the
// wiper moves so those readings can be flagged rather than lost.
typedef enum {
  MOTOR_STATE_IDLE = 0,
  MOTOR_STATE_TO_SWEEP, // travelling out to the sweep endpoint
  MOTOR_STATE_TO_PARK,  // travelling back to park
} motorState_e;

static motorState_e motorState = MOTOR_STATE_IDLE;
static uint8_t motorSweepsRemaining = 0;
static uint32_t motorNextStepMs = 0;

// Current accumulators for the cycle in progress.
static uint32_t cycleStartMs = 0;
static uint32_t cyclePeakMa = 0;
static uint64_t cycleSumMa = 0;
static uint32_t cycleSamples = 0;

bool motorIsBusy(void) { return (motorState != MOTOR_STATE_IDLE); }

bool motorStartCleaningCycle(uint8_t sweeps) {
  if (!motorReady) {
    printf("[motor] Cleaning cycle skipped, controller not initialized.\n");
    return false;
  }
  if (motorIsBusy()) {
    printf("[motor] Cleaning cycle skipped, one is already running.\n");
    return false;
  }
  if (sweeps == 0) {
    sweeps = motorCfg.sweeps;
  }

  /* Baseline the bus current BEFORE the servo is commanded, so the wipe's own
   * draw is reported against what the node was drawing a moment earlier rather
   * than against an assumed idle figure. Everything else on the board - the
   * radio, the ADC, the LEDs - moves that baseline around. */
  uint16_t baseline = 0;
  lastWipe.baseline_ma = readBusMagnitudeMa(baseline) ? baseline : 0;

  printf("[motor] Starting cleaning cycle, %u sweep(s), %u -> %u us.\n", (unsigned)sweeps,
         motorCfg.park_us, motorCfg.sweep_us);
  if (!motorSetPulseUs(motorCfg.sweep_us)) {
    printf("[motor] Failed to command the first sweep.\n");
    return false;
  }

  cycleStartMs = (uint32_t)uptimeGetMs();
  cyclePeakMa = 0;
  cycleSumMa = 0;
  cycleSamples = 0;

  motorSweepsRemaining = sweeps;
  motorState = MOTOR_STATE_TO_SWEEP;
  motorNextStepMs = cycleStartMs + motorCfg.travel_ms;
  return true;
}

/*! Close out a cycle: stop the servo and publish the telemetry. */
static void motorFinishCycle(bool aborted) {
  motorState = MOTOR_STATE_IDLE;
  motorRelease();

  lastWipe.valid = true;
  lastWipe.count++;
  lastWipe.start_uptime_s = cycleStartMs / 1000;
  lastWipe.end_uptime_s = (uint32_t)(uptimeGetMs() / 1000);
  const uint32_t dur = (uint32_t)uptimeGetMs() - cycleStartMs;
  lastWipe.duration_ms = (dur > UINT16_MAX) ? UINT16_MAX : (uint16_t)dur;
  lastWipe.peak_ma = (cyclePeakMa > UINT16_MAX) ? UINT16_MAX : (uint16_t)cyclePeakMa;
  lastWipe.mean_ma = cycleSamples ? (uint16_t)(cycleSumMa / (uint64_t)cycleSamples) : 0;
  lastWipe.samples = (cycleSamples > UINT16_MAX) ? UINT16_MAX : (uint16_t)cycleSamples;
  lastWipe.stalled = (lastWipe.peak_ma >= motorCfg.stall_ma);

  printf("[motor] Cycle %u %s: %u ms, base %u mA, mean %u mA, peak %u mA, n %u%s\n",
         lastWipe.count, aborted ? "ABORTED" : "complete", lastWipe.duration_ms,
         lastWipe.baseline_ma, lastWipe.mean_ma, lastWipe.peak_ma, lastWipe.samples,
         lastWipe.stalled ? "  *** STALL THRESHOLD EXCEEDED ***" : "");

  /* A cycle that drew no more than it was drawing at rest means the telemetry is
   * measuring the wrong thing. Worth saying out loud: silently reporting a flat
   * zero peak for a whole deployment would look like a working wiper right up
   * until someone tried to use the numbers. */
  if (lastWipe.samples &&
      lastWipe.peak_ma <= (uint32_t)lastWipe.baseline_ma + MOTOR_MIN_WIPE_DELTA_MA) {
    printf("[motor] WARN no current rise during the cycle (base %u, peak %u mA). Either the "
           "servo is not drawing, or the INA232 at 0x%02X is not the monitor carrying the "
           "servo rail - compare both with `chl wipe --all` and set chlPwrAddr.\n",
           lastWipe.baseline_ma, lastWipe.peak_ma, (unsigned)busMonitorAddr);
  }
}

void motorService(void) {
  if (motorState == MOTOR_STATE_IDLE) {
    return;
  }

  /* Sample the bus current on every call while the cycle runs. loop() runs at
   * roughly 100 Hz, so a default 800 ms leg yields on the order of 70 readings.
   * A failed read is skipped rather than counted as zero, which would drag the
   * mean down and hide a problem. */
  uint16_t ma = 0;
  if (readBusMagnitudeMa(ma)) {
    if (ma > cyclePeakMa) {
      cyclePeakMa = ma;
    }
    cycleSumMa += ma;
    cycleSamples++;
  }

  // Unsigned subtraction so this still behaves correctly across a tick rollover.
  if (((uint32_t)uptimeGetMs() - motorNextStepMs) > (UINT32_MAX / 2)) {
    return; // not time yet
  }

  switch (motorState) {
  case MOTOR_STATE_TO_SWEEP:
    // Reached the far end, head back to park.
    if (!motorSetPulseUs(motorCfg.park_us)) {
      printf("[motor] Failed to command sweep-back, aborting cycle.\n");
      motorFinishCycle(true);
      return;
    }
    motorState = MOTOR_STATE_TO_PARK;
    motorNextStepMs = (uint32_t)uptimeGetMs() + motorCfg.travel_ms;
    break;

  case MOTOR_STATE_TO_PARK:
    // Back at park, so one out-and-back pass is done.
    motorSweepsRemaining--;
    if (motorSweepsRemaining > 0) {
      if (!motorSetPulseUs(motorCfg.sweep_us)) {
        printf("[motor] Failed to command sweep-out, aborting cycle.\n");
        motorFinishCycle(true);
        return;
      }
      motorState = MOTOR_STATE_TO_SWEEP;
      motorNextStepMs = (uint32_t)uptimeGetMs() + motorCfg.travel_ms;
    } else {
      // Let go once parked so the servo is not holding torque between cycles.
      motorFinishCycle(false);
    }
    break;

  case MOTOR_STATE_IDLE:
  default:
    break;
  }
}
