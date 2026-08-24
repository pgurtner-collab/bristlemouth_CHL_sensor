#include "ads1115.h"
#include "debug.h"

ADS1115::ADS1115(I2CInterface_t *interface, uint8_t address) {
  _interface = interface;
  _addr = address;
}

/*!
 Put the ADS1115 into continuous-conversion mode on the AIN0/AIN1
 differential input at +-4.096V full scale, and verify the config register
 reads back as set.

 \return true if successful, false otherwise
*/
bool ADS1115::init() {
  bool rval = false;
  printf("ADS1115 init\n");

  uint8_t retriesRemaining = 3;
  while (!rval && retriesRemaining--) {
    if (!writeReg(ADS1115_REG_POINTER_CONFIG, ADS1115_CONFIG_CONTINUOUS_DIFF01_4V096_128SPS)) {
      printf("ADS1115 init failed to write config, retry - %u\n", retriesRemaining);
      continue;
    }
    uint16_t readBackConfig = 0;
    if (!readReg(ADS1115_REG_POINTER_CONFIG, readBackConfig)) {
      printf("ADS1115 init failed to read back config, retry - %u\n", retriesRemaining);
      continue;
    }
    // Ignore the OS bit (15) -- its meaning is inverted between write and
    // read, and it is not meaningful for us while in continuous mode.
    if ((readBackConfig & 0x7FFF) != (ADS1115_CONFIG_CONTINUOUS_DIFF01_4V096_128SPS & 0x7FFF)) {
      printf("ADS1115 init config mismatch, read 0x%04X, retry - %u\n", readBackConfig,
             retriesRemaining);
      continue;
    }
    printf("ADS1115 initialized!\n");
    rval = true;
  }

  return rval;
}

/*!
 Read the latest conversion result and convert it to volts, using the
 +-4.096V full-scale range configured in init().

 \param[out] voltage Latest conversion result, in volts
 \return true if successful, false otherwise
*/
bool ADS1115::readVoltage(float &voltage) {
  uint16_t rawCounts = 0;
  if (!readReg(ADS1115_REG_POINTER_CONVERSION, rawCounts)) {
    return false;
  }
  voltage = (float)((int16_t)rawCounts) * ADS1115_LSB_VOLTS_4V096;
  return true;
}

/*!
  Read a 16-bit register from the device by writing its pointer register
  address, then reading back 2 bytes (MSB first).

  \param[in] reg Pointer register address (ADS1115_REG_POINTER_*)
  \param[out] value Register value, in host byte order
  \return true if successful, false otherwise
*/
bool ADS1115::readReg(uint8_t reg, uint16_t &value) {
  if (writeBytes(&reg, sizeof(reg), 100) != I2C_OK) {
    return false;
  }
  uint16_t rawBytes = 0;
  if (readBytes((uint8_t *)&rawBytes, sizeof(rawBytes), 100) != I2C_OK) {
    return false;
  }
  value = __builtin_bswap16(rawBytes);
  return true;
}

/*!
  Write a 16-bit register to the device: pointer register address byte
  followed by 2 data bytes (MSB first).

  \param[in] reg Pointer register address (ADS1115_REG_POINTER_*)
  \param[in] value Register value to set, in host byte order
  \return true if successful, false otherwise
*/
bool ADS1115::writeReg(uint8_t reg, uint16_t value) {
  uint8_t bytes[] = {reg, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF)};
  return (writeBytes(bytes, sizeof(bytes), 100) == I2C_OK);
}
