#include "ads1115.h"
#include "FreeRTOS.h"
#include "debug.h"
#include "task.h"

ADS1115::ADS1115(I2CInterface_t *interface, uint8_t address) {
  _interface = interface;
  _addr = address;
}

/*!
 Configure the ADS1115 for single-shot conversions on the AIN0/AIN1 differential
 input at +-4.096V full scale, and verify the config register reads back as set.

 No conversion is started here -- readVoltage() triggers each one.

 \return true if successful, false otherwise
*/
bool ADS1115::init() {
  bool rval = false;
  printf("ADS1115 init\n");

  uint8_t retriesRemaining = 3;
  while (!rval && retriesRemaining--) {
    if (!writeReg(ADS1115_REG_POINTER_CONFIG, ADS1115_CONFIG_SINGLESHOT_DIFF01_4V096_128SPS)) {
      printf("ADS1115 init failed to write config, retry - %u\n", retriesRemaining);
      continue;
    }
    uint16_t readBackConfig = 0;
    if (!readReg(ADS1115_REG_POINTER_CONFIG, readBackConfig)) {
      printf("ADS1115 init failed to read back config, retry - %u\n", retriesRemaining);
      continue;
    }
    // Ignore the OS bit (15) -- its meaning is inverted between write and read, so it
    // never matches what we wrote.
    if ((readBackConfig & 0x7FFF) != (ADS1115_CONFIG_SINGLESHOT_DIFF01_4V096_128SPS & 0x7FFF)) {
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
 Trigger one conversion, wait for it to complete, and convert the result to volts
 using the +-4.096V full-scale range configured in init().

 Because the conversion is explicitly started and its completion confirmed via the
 OS bit, a value returned here cannot be a stale leftover from an earlier read.

 \param[out] voltage Conversion result, in volts
 \return true if successful, false otherwise
*/
bool ADS1115::readVoltage(float &voltage) {
  // Writing OS=1 starts a single conversion.
  if (!writeReg(ADS1115_REG_POINTER_CONFIG, ADS1115_CONFIG_SINGLESHOT_DIFF01_4V096_128SPS)) {
    printf("ADS1115 failed to start a conversion.\n");
    return false;
  }

  // Wait for the device to report itself idle again, meaning the result is ready.
  bool conversionComplete = false;
  for (uint8_t tries = 0; tries < ADS1115_CONVERSION_POLL_TRIES; tries++) {
    vTaskDelay(pdMS_TO_TICKS(ADS1115_CONVERSION_POLL_MS));
    uint16_t config = 0;
    if (!readReg(ADS1115_REG_POINTER_CONFIG, config)) {
      printf("ADS1115 failed to read conversion status.\n");
      return false;
    }
    if (config & ADS1115_CONFIG_OS_MASK) {
      conversionComplete = true;
      break;
    }
  }
  if (!conversionComplete) {
    printf("ADS1115 conversion did not complete within %u ms.\n",
           (unsigned)(ADS1115_CONVERSION_POLL_MS * ADS1115_CONVERSION_POLL_TRIES));
    return false;
  }

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
