#pragma once

#include "abstract/abstract_i2c.h"

// 7-bit address, selected by what the ADDR pin is tied to.
#define ADS1115_ADDR_GND 0x48
#define ADS1115_ADDR_VDD 0x49
#define ADS1115_ADDR_SDA 0x4A
#define ADS1115_ADDR_SCL 0x4B

#define ADS1115_REG_POINTER_CONVERSION 0x00
#define ADS1115_REG_POINTER_CONFIG 0x01

// Config register value: single-shot conversion, AIN0-AIN1 differential input,
// +-4.096V full-scale range, 128 SPS, comparator disabled. MODE (bit 8) = 1 selects
// single-shot, so the device converts once and then powers back down rather than
// running continuously. Writing OS (bit 15) = 1 is what triggers each conversion.
#define ADS1115_CONFIG_SINGLESHOT_DIFF01_4V096_128SPS 0x8383

// Same settings but with MODE (bit 8) = 0, i.e. continuous conversion. Kept for
// reference; the driver uses single-shot so that every reading is one it explicitly
// asked for, which makes a stale or frozen conversion register obvious.
#define ADS1115_CONFIG_CONTINUOUS_DIFF01_4V096_128SPS 0x8283

// OS is bit 15 of the config register. Written as 1 it starts a conversion. Read
// back, 0 means a conversion is in progress and 1 means the device is idle/done.
#define ADS1115_CONFIG_OS_MASK 0x8000

// A conversion at 128 SPS takes ~7.8ms. Poll a bit longer than that before giving up.
#define ADS1115_CONVERSION_POLL_MS 2
#define ADS1115_CONVERSION_POLL_TRIES 15

// Volts per ADC count for the +-4.096V full-scale range (4.096V / 2^15 counts).
#define ADS1115_LSB_VOLTS_4V096 0.000125f

// Driver for the TI ADS1115 16-bit I2C ADC, used here to read the analog
// output of a chlorophyll sensor board wired to the AIN0/AIN1 differential
// input.
class ADS1115 : public AbstractI2C {
public:
  ADS1115(I2CInterface_t *interface, uint8_t address = ADS1115_ADDR_GND);

  bool init();

  /*! Address currently in use. */
  uint8_t getAddress() const { return (uint8_t)_addr; }

  bool readVoltage(float &voltage);

private:
  bool readReg(uint8_t reg, uint16_t &value);
  bool writeReg(uint8_t reg, uint16_t value);
};
