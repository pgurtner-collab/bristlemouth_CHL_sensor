#pragma once

#include "abstract/abstract_i2c.h"

#define ADS1115_ADDR_GND 0x48 // default 7-bit address, ADDR pin tied to GND

#define ADS1115_REG_POINTER_CONVERSION 0x00
#define ADS1115_REG_POINTER_CONFIG 0x01

// Config register value: continuous conversion, AIN0-AIN1 differential
// input, +-4.096V full-scale range, 128 SPS, comparator disabled.
// OS (bit 15) is set to 1 here -- the device powers up in single-shot/
// power-down mode, and writing OS=1 is what triggers it to start
// converting. Once running, continuous mode (MODE=0) keeps it converting
// indefinitely with no further OS writes needed.
#define ADS1115_CONFIG_CONTINUOUS_DIFF01_4V096_128SPS 0x8283

// Volts per ADC count for the +-4.096V full-scale range (4.096V / 2^15 counts).
#define ADS1115_LSB_VOLTS_4V096 0.000125f

// Driver for the TI ADS1115 16-bit I2C ADC, used here to read the analog
// output of a chlorophyll sensor board wired to the AIN0/AIN1 differential
// input.
class ADS1115 : public AbstractI2C {
public:
  ADS1115(I2CInterface_t *interface, uint8_t address = ADS1115_ADDR_GND);

  bool init();
  bool readVoltage(float &voltage);

private:
  bool readReg(uint8_t reg, uint16_t &value);
  bool writeReg(uint8_t reg, uint16_t value);
};
