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
 Configure the PCA9685 for servo use.

 IMPORTANT - the ALL-CALL address. Out of reset the PCA9685 answers to 0x70 in
 addition to its own address, and 0x70 is the TCA9546A I2C mux on this board.
 Worse, Bristlefin's TCA9546A::setChannel() reads the mux back to verify every
 channel change, so two devices would drive the bus at once. Clearing the ALLCALL
 bit in MODE1 is therefore the very first thing done here.

 This only protects the bus from the moment this runs. MODE1 resets to
 ALLCALL-enabled on every power-up, so anything that touches the mux before this
 point is still exposed. See the wiring notes for how to avoid that window.

 \return true if successful, false otherwise
*/
bool PCA9685::init() {
  printf("PCA9685 init\n");

  // Sleep is required before the prescaler can be changed. Write ALLCALL=0 in the
  // same operation so the 0x70 conflict is cleared as early as possible.
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
    printf("PCA9685 ALLCALL still enabled (MODE1 0x%02X) - it will fight the mux at "
           "0x70.\n",
           mode1);
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
// Cleaning motor
// ---------------------------------------------------------------------------

static PCA9685 servoController(&i2c1, PCA9685_ADDR_DEFAULT);
static bool motorReady = false;

bool motorIsReady(void) { return motorReady; }

bool motorInit(void) {
  motorReady = servoController.init();
  if (!motorReady) {
    printf("ERROR - Failed to initialize PCA9685 servo controller!\n");
    return false;
  }

  // Park the wiper so it starts from a known position.
  if (!motorSetAngle(MOTOR_CLEAN_PARK_DEG)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(MOTOR_SWEEP_TRAVEL_MS));
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

  return servoController.setChannelPulseUs(MOTOR_SERVO_CHANNEL, pulse_us);
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
  MOTOR_STATE_TO_SWEEP, // travelling out to MOTOR_CLEAN_SWEEP_DEG
  MOTOR_STATE_TO_PARK,  // travelling back to MOTOR_CLEAN_PARK_DEG
} motorState_e;

static motorState_e motorState = MOTOR_STATE_IDLE;
static uint8_t motorSweepsRemaining = 0;
static uint32_t motorNextStepMs = 0;

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
    return false;
  }

  printf("[motor] Starting cleaning cycle, %u sweep(s).\n", (unsigned)sweeps);
  if (!motorSetAngle(MOTOR_CLEAN_SWEEP_DEG)) {
    printf("[motor] Failed to command the first sweep.\n");
    return false;
  }

  motorSweepsRemaining = sweeps;
  motorState = MOTOR_STATE_TO_SWEEP;
  motorNextStepMs = (uint32_t)uptimeGetMs() + MOTOR_SWEEP_TRAVEL_MS;
  return true;
}

void motorService(void) {
  if (motorState == MOTOR_STATE_IDLE) {
    return;
  }

  // Unsigned subtraction so this still behaves correctly across a tick rollover.
  if (((uint32_t)uptimeGetMs() - motorNextStepMs) > (UINT32_MAX / 2)) {
    return; // not time yet
  }

  switch (motorState) {
  case MOTOR_STATE_TO_SWEEP:
    // Reached the far end, head back to park.
    if (!motorSetAngle(MOTOR_CLEAN_PARK_DEG)) {
      printf("[motor] Failed to command sweep-back, aborting cycle.\n");
      motorState = MOTOR_STATE_IDLE;
      motorRelease();
      return;
    }
    motorState = MOTOR_STATE_TO_PARK;
    motorNextStepMs = (uint32_t)uptimeGetMs() + MOTOR_SWEEP_TRAVEL_MS;
    break;

  case MOTOR_STATE_TO_PARK:
    // Back at park, so one out-and-back pass is done.
    motorSweepsRemaining--;
    if (motorSweepsRemaining > 0) {
      if (!motorSetAngle(MOTOR_CLEAN_SWEEP_DEG)) {
        printf("[motor] Failed to command sweep-out, aborting cycle.\n");
        motorState = MOTOR_STATE_IDLE;
        motorRelease();
        return;
      }
      motorState = MOTOR_STATE_TO_SWEEP;
      motorNextStepMs = (uint32_t)uptimeGetMs() + MOTOR_SWEEP_TRAVEL_MS;
    } else {
      motorState = MOTOR_STATE_IDLE;
      // Let go once parked so the servo is not holding torque between cycles.
      motorRelease();
      printf("[motor] Cleaning cycle complete.\n");
    }
    break;

  case MOTOR_STATE_IDLE:
  default:
    break;
  }
}
