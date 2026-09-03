#pragma once

#include <stdbool.h>
#include <stdint.h>

// Cleaning motor: a goBILDA Proton servo driven by a PCA9685 I2C PWM controller
// sitting on the same I2C bus as the ADS1115.
//
// Servo specs (goBILDA Proton, steel gears, 180 deg):
//   pulse width  600 - 2400 us
//   travel       177 deg max, 0.098 deg/us
//   supply       4.8 - 6.0 V, 80-90 mA no load, 800-1000 mA stall
//
// The 2400-600 = 1800 us span at 0.098 deg/us works out to 176.4 deg, which
// matches the quoted 177 deg, so the pulse range and travel are consistent.
//
// Sweep endpoints are held as PULSE WIDTHS, not angles. The wiper arm's useful
// travel is set by where the brush sits on the optical window, which is a
// mechanical fact about how the horn was clocked onto the spline - it has no
// fixed relationship to servo degrees. Re-seating the brush changes it. Pulse
// widths are what the CLI jog command reports, so what you read off the bench is
// what you store, with no angle conversion in between to get wrong.

// PCA9685 7-bit I2C address. 0x40 is the default with all address jumpers open.
#define PCA9685_ADDR_DEFAULT 0x40

// PWM refresh rate for the servo. Standard hobby-servo framing.
#define MOTOR_PWM_FREQ_HZ 50

// Which PCA9685 output the servo signal wire is on (0-15).
#define MOTOR_SERVO_CHANNEL 0

// Hard limits from the goBILDA spec sheet. Commands are clamped to these.
#define MOTOR_MIN_PULSE_US 600
#define MOTOR_MAX_PULSE_US 2400
#define MOTOR_RANGE_DEG 177.0f

/* Defaults, all overridable at runtime - see the config table in user_code.cpp.
 * The park/sweep values below correspond to the 45 deg / 135 deg endpoints the
 * original code used, and are only a starting point: measure the real ones with
 * `chl jog` after any change to the brush or horn. */
#define MOTOR_DEFAULT_PARK_US 1058  // ~45 deg
#define MOTOR_DEFAULT_SWEEP_US 1973 // ~135 deg
#define MOTOR_DEFAULT_TRAVEL_MS 800
#define MOTOR_DEFAULT_SWEEPS 2

/* Least current rise, over the pre-cycle baseline, that counts as the servo
 * having actually drawn something. Below this the telemetry is assumed to be
 * pointed at the wrong INA232 rather than at a servo that used no power. */
#define MOTOR_MIN_WIPE_DELTA_MA 10

/*! Per-wipe telemetry, captured while the servo is actually moving. */
typedef struct {
  bool valid;             //!< false until the first wipe has completed
  bool stalled;           //!< peak exceeded the configured stall threshold
  uint32_t start_uptime_s;//!< when the cycle began
  uint32_t end_uptime_s;  //!< when it finished
  uint16_t duration_ms;   //!< measured, not the commanded travel time
  uint16_t baseline_ma;   //!< bus current just before the servo was commanded
  uint16_t peak_ma;       //!< highest single reading during the cycle
  uint16_t mean_ma;       //!< mean over the cycle
  uint16_t samples;       //!< current readings that went into the above
  uint16_t count;         //!< total cycles since boot
} motorWipeStats_t;

/*! Runtime-settable motion parameters. */
typedef struct {
  uint16_t park_us;
  uint16_t sweep_us;
  uint16_t travel_ms;
  uint8_t sweeps;
  uint16_t stall_ma;
} motorConfig_t;

/*!
 Bring up the PCA9685 and park the servo.

 Also disables the PCA9685's ALL-CALL address, which defaults to 0x70 and would
 otherwise collide with the TCA9546A I2C mux. See the note in motor_code.cpp.

 \param[in] cfg Motion parameters to adopt
 \return true if the controller was found and configured
*/
bool motorInit(const motorConfig_t &cfg);

/*! Replace the motion parameters. Safe to call while idle; ignored mid-cycle. */
bool motorSetConfig(const motorConfig_t &cfg);
const motorConfig_t &motorGetConfig(void);

/*! Command an absolute angle, 0 to MOTOR_RANGE_DEG. Convenience for the CLI. */
bool motorSetAngle(float degrees);

/*! Command a raw pulse width, clamped to the servo's 600-2400 us range. */
bool motorSetPulseUs(uint16_t pulse_us);

/*! Last pulse width commanded, whether by a cycle or by the CLI. */
uint16_t motorGetPulseUs(void);

/*!
 Stop driving the PWM output entirely. The servo stops holding position and goes
 limp, which draws far less current than fighting a stalled wiper.
*/
bool motorRelease(void);

/*!
 Begin a wiper cleaning cycle and return immediately.

 One "sweep" is an out-and-back pass, so sweeps = 2 traces
 park -> sweep -> park -> sweep -> park.

 This does NOT block. Call motorService() regularly to actually advance the
 movement, which keeps the chlorophyll sampling running while the wiper moves.

 \param[in] sweeps Number of out-and-back passes, or 0 to use the configured count
 \return true if the cycle was started
*/
bool motorStartCleaningCycle(uint8_t sweeps);

/*!
 Advance an in-progress cleaning cycle and, while one is running, sample the bus
 current. Cheap to call and does nothing when idle, so call it every pass through
 the main loop - the current sampling resolution is set by how often you do.
*/
void motorService(void);

/*! True while a cleaning cycle is in progress and the wiper may be moving. */
bool motorIsBusy(void);

/*! True if motorInit() succeeded. */
bool motorIsReady(void);

/*! Telemetry from the most recently completed cycle. */
const motorWipeStats_t &motorGetLastWipe(void);

/*!
 Choose which INA232 the per-wipe current figures are read from.

 0x43 (I2C_INA_MAIN_ADDR) is the mote's own monitor; 0x41 (I2C_INA_PODL_ADDR) is
 the power-over-data-line monitor. Which of the two actually carries the servo's
 draw depends on where the servo rail is taken off, so it is left configurable
 and settled on the bench rather than assumed here.
*/
void motorSetPowerMonitorAddress(uint8_t address);

/*!
 Clear the PCA9685's ALL-CALL address as early as possible.

 Standalone from motorInit() because it needs to happen before anything else
 talks to the I2C mux at 0x70, and motorInit() also parks the servo, which needs
 the servo rail up. Safe to call more than once.
*/
bool motorClearAllCall(void);
