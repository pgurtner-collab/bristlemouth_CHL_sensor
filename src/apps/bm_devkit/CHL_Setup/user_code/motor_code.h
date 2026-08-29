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

// PCA9685 7-bit I2C address. 0x40 is the default with all address jumpers open.
#define PCA9685_ADDR_DEFAULT 0x40

// PWM refresh rate for the servo. Standard hobby-servo framing.
#define MOTOR_PWM_FREQ_HZ 50

// Which PCA9685 output the servo signal wire is on (0-15).
#define MOTOR_SERVO_CHANNEL 0

// Servo travel limits, from the goBILDA spec sheet.
#define MOTOR_MIN_PULSE_US 600
#define MOTOR_MAX_PULSE_US 2400
#define MOTOR_RANGE_DEG 177.0f

// Wiper sweep endpoints. Well inside the mechanical limits, so a slightly
// miscalibrated horn cannot drive the servo into a hard stop and stall it.
#define MOTOR_CLEAN_PARK_DEG 45.0f
#define MOTOR_CLEAN_SWEEP_DEG 135.0f

// How long to allow the servo to travel between sweep endpoints, in ms. The
// Proton needs roughly 0.2 s per 60 deg unloaded, so a full sweep plus margin.
#define MOTOR_SWEEP_TRAVEL_MS 800

/*!
 Bring up the PCA9685 and park the servo.

 Also disables the PCA9685's ALL-CALL address, which defaults to 0x70 and would
 otherwise collide with the TCA9546A I2C mux. See the note in motor_code.cpp.

 \return true if the controller was found and configured
*/
bool motorInit(void);

/*! Command the servo to an absolute angle, 0 to MOTOR_RANGE_DEG degrees. */
bool motorSetAngle(float degrees);

/*! Command a raw pulse width, clamped to the servo's 600-2400 us range. */
bool motorSetPulseUs(uint16_t pulse_us);

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

 \param[in] sweeps Number of out-and-back passes
 \return true if the cycle was started
*/
bool motorStartCleaningCycle(uint8_t sweeps);

/*!
 Advance an in-progress cleaning cycle. Cheap to call and does nothing when idle,
 so call it every pass through the main loop.
*/
void motorService(void);

/*! True while a cleaning cycle is in progress and the wiper may be moving. */
bool motorIsBusy(void);

/*! True if motorInit() succeeded. */
bool motorIsReady(void);
