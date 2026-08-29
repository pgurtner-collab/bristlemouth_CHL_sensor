#include "user_code.h"
#include "ads1115.h"
#include "app_util.h"
#include "bristlefin.h"
#include "bsp.h"
#include "debug.h"
#include "sensors.h"
#include "spotter.h"
#include "stm32_rtc.h"
#include "uptime.h"

#define LED_ON_TIME_MS 20
#define LED_PERIOD_MS 1000
#define CHL_READ_PERIOD_MS 2000

// Chlorophyll concentration calibration, applied to the measured voltage:
//   CHL concentration = [(Measured Voltage - 0.0235) * 25.4520] ug/L
#define CHL_VOLTAGE_OFFSET_V 0.0235f
#define CHL_UG_PER_L_PER_VOLT 25.4520f

//pin config
//VDD vto 3V3 BM rail
//GND to BM GND
//SCL to BM I2C SCL
//SDA to BM I2C SDA
//ADDR to BM GND, giving 0x48 (VDD = 0x49, SDA = 0x4A, SCL = 0x4B).
//NOTE: the board shows up on I2C mux channel 2. Do not confuse it with the 0x4A
//  device on channel 1, which is on-board hardware, not this ADC.
//A0 to CHL sensor High output
//A1 to CHL sensor Sense GND output




// ADS1115 ADC, on the main I2C bus, reading the chlorophyll board's
// analog output on its AIN0/AIN1 differential input.
static ADS1115 chlSensor(&i2c1, ADS1115_ADDR_GND);

void setup(void) {
  // Enable the input to the Vout power supply. Without this, Vout and 5V stay off.
  bristlefin.enableVbus();
  // ensure Vbus stable before enable Vout with a 5ms delay.
  vTaskDelay(pdMS_TO_TICKS(5));
  // enable Vout, 12V by default.
  bristlefin.enableVout();
  bristlefin.enable3V();

  // Let the ADS1115's supply come up before talking to it.
  vTaskDelay(pdMS_TO_TICKS(50));

  if (!chlSensor.init()) {
    printf("ERROR - Failed to initialize ADS1115 chlorophyll sensor!\n");
  }
}

void loop(void) {
  /* USER LOOP CODE GOES HERE */
  // Read the chlorophyll sensor every CHL_READ_PERIOD_MS and log it.
  static uint32_t chlReadTimer = uptimeGetMs();
  if ((uint32_t)uptimeGetMs() - chlReadTimer >= CHL_READ_PERIOD_MS) {
    chlReadTimer = uptimeGetMs();

    float chlVoltage = 0.0f;
    if (chlSensor.readVoltage(chlVoltage)) {
      // Convert the measured voltage to a chlorophyll concentration in ug/L.
      const float chlConcentration = (chlVoltage - CHL_VOLTAGE_OFFSET_V) * CHL_UG_PER_L_PER_VOLT;

      // Get the RTC if available
      RTCTimeAndDate_t time_and_date = {};
      rtcGet(&time_and_date);
      char rtcTimeBuffer[32];
      rtcPrint(rtcTimeBuffer, &time_and_date);

      // Log the reading to a file, to the spotter_log_console console, and to the printf console.
      spotter_log(0, "chl_data.log", USE_TIMESTAMP, "tick: %llu, rtc: %s, chl_ugl: %.6f\n",
                  uptimeGetMs(), rtcTimeBuffer, chlConcentration);
      spotter_log_console(0, "[chl] | tick: %llu, rtc: %s, chl_ugl: %.6f", uptimeGetMs(),
                          rtcTimeBuffer, chlConcentration);
      printf("[chl] | tick: %llu, rtc: %s, chl_ugl: %.6f\n", uptimeGetMs(), rtcTimeBuffer,
             chlConcentration);
    } else {
      printf("ERROR - Failed to read ADS1115 chlorophyll sensor!\n");
    }
  }

  // Heartbeat LED1, blinks green every LED_PERIOD_MS milliseconds.
  static bool ledState = false;
  static uint64_t ledLastScheduledOnTime = uptimeGetMs();
  const uint64_t elapsedSinceOnTime = uptimeGetMs() - ledLastScheduledOnTime;
  if (!ledState && elapsedSinceOnTime >= LED_PERIOD_MS) {
    ledLastScheduledOnTime += LED_PERIOD_MS;
    bristlefin.setLed(1, Bristlefin::LED_GREEN);
    ledState = true;
  } else if (ledState && elapsedSinceOnTime >= LED_ON_TIME_MS) {
    bristlefin.setLed(1, Bristlefin::LED_OFF);
    ledState = false;
  }
}
