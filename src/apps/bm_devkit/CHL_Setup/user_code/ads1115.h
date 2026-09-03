#pragma once

#include "abstract/abstract_i2c.h"

// 7-bit address, selected by what the ADDR pin is tied to.
#define ADS1115_ADDR_GND 0x48
#define ADS1115_ADDR_VDD 0x49
#define ADS1115_ADDR_SDA 0x4A
#define ADS1115_ADDR_SCL 0x4B

#define ADS1115_REG_POINTER_CONVERSION 0x00
#define ADS1115_REG_POINTER_CONFIG 0x01

/* Config register bit layout (TI ADS1115 datasheet, table 8-4):
 *   [15]    OS         1 on write starts a conversion; 1 on read means idle
 *   [14:12] MUX        000 = AIN0-AIN1 differential (what we use)
 *   [11:9]  PGA        full-scale range, see ads1115_pga_e
 *   [8]     MODE       1 = single-shot, 0 = continuous
 *   [7:5]   DR         100 = 128 SPS
 *   [4:0]   comparator 00011 = disabled
 *
 * Everything except PGA is fixed, so the base value below has PGA = 0 and
 * the selected range is OR'd in. */
#define ADS1115_CONFIG_BASE 0x8183 // OS=1, MUX=AIN0-AIN1, MODE=single-shot, 128SPS, comp off
#define ADS1115_CONFIG_PGA_SHIFT 9
#define ADS1115_CONFIG_PGA_MASK 0x0E00

// OS is bit 15. Written as 1 it starts a conversion. Read back, 0 means a
// conversion is in progress and 1 means the device is idle/done.
#define ADS1115_CONFIG_OS_MASK 0x8000

// A conversion at 128 SPS takes ~7.8ms. Poll a bit longer than that before giving up.
#define ADS1115_CONVERSION_POLL_MS 2
#define ADS1115_CONVERSION_POLL_TRIES 15

/* Full-scale range selection.
 *
 * IMPORTANT: the PGA setting does NOT change what voltage the part can safely
 * accept. The absolute maximum on any analog input is VDD + 0.3 V regardless of
 * range, so selecting +-6.144 V on a 3.3 V supply does not make a 5 V signal
 * readable - it just clips at the supply while the input protection diodes
 * conduct. Pick the smallest range that still covers the real signal swing:
 * a smaller range is quieter, because the LSB shrinks with it. */
typedef enum {
  ADS1115_PGA_6V144 = 0,
  ADS1115_PGA_4V096 = 1,
  ADS1115_PGA_2V048 = 2,
  ADS1115_PGA_1V024 = 3,
  ADS1115_PGA_0V512 = 4,
  ADS1115_PGA_0V256 = 5,
  ADS1115_PGA_COUNT
} ads1115_pga_e;

// Counts at or beyond this are close enough to the +-32767 rail that the
// reading should be treated as clipped rather than as a measurement.
#define ADS1115_SATURATION_COUNTS 32000

// Driver for the TI ADS1115 16-bit I2C ADC, used here to read the analog
// output of a chlorophyll sensor board wired to the AIN0/AIN1 differential
// input.
class ADS1115 : public AbstractI2C {
public:
  ADS1115(I2CInterface_t *interface, uint8_t address = ADS1115_ADDR_GND);

  /*! Configure the part. Applies whatever range setPga() last selected. */
  bool init();

  /*! Address currently in use. */
  uint8_t getAddress() const { return (uint8_t)_addr; }

  /*!
   Select the full-scale range. Takes effect on the next conversion; safe to call
   at any time. Out-of-range values are rejected rather than silently clamped.
  */
  bool setPga(ads1115_pga_e pga);
  ads1115_pga_e getPga() const { return _pga; }

  /*! Full-scale voltage of the range currently selected, for logging. */
  float getFullScaleVolts() const;

  /*! Volts per count of the range currently selected. */
  float getLsbVolts() const;

  /*!
   Trigger one conversion and return the result.

   \param[out] voltage Result in volts
   \param[out] counts  Raw signed conversion result, or nullptr if not wanted.
                       Worth logging: it is the only way to tell a genuine
                       near-full-scale reading from a clipped one.
   \return true if the conversion completed
  */
  bool readVoltage(float &voltage, int16_t *counts = nullptr);

  /*! True if the last reading was close enough to the rail to be suspect. */
  bool lastReadSaturated() const { return _saturated; }

private:
  bool readReg(uint8_t reg, uint16_t &value);
  bool writeReg(uint8_t reg, uint16_t value);
  uint16_t configWord() const;

  ads1115_pga_e _pga = ADS1115_PGA_4V096;
  bool _saturated = false;
};
