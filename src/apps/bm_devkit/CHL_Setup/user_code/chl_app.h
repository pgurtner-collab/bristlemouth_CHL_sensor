#pragma once

#include "ads1115.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * The handful of things the `chl` console command needs from the application.
 * Kept deliberately small - the CLI is a bench tool, not a second control path.
 */

/*! One ADC conversion, converted every way we care about. */
typedef struct {
  bool ok;
  int16_t counts;   //!< raw signed conversion result
  float volts;      //!< differential volts at AIN0-AIN1
  float ugl;        //!< chlorophyll, after the Turner calibration
  bool saturated;   //!< reading was at or near the ADC rail
} chlReading_t;

/*! Take a single reading on demand. */
chlReading_t chlAppReadOnce(void);

/*! Push the current chlCfg values into the ADC and motor drivers. */
void chlAppApplyConfig(void);

/*! Close the averaging window now: log, transmit, and start a fresh one. */
void chlAppReportNow(void);

/*! Print sensor, wiper and link status. */
void chlAppStatus(void);

/*! Probe the I2C addresses this app depends on. */
void chlAppScanI2C(void);

/*! Register the `chl` console command. Call once from setup(). */
void chlCliInit(void);
