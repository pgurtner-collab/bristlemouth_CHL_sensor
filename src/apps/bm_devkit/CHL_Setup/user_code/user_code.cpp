#include "user_code.h"
#include "ads1115.h"
#include "app_util.h"
#include "bristlefin.h"
#include "bsp.h"
#include "debug.h"
#include "spotter.h"
#include "stm32_rtc.h"
#include "uptime.h"

#define LED_ON_TIME_MS 20
#define LED_PERIOD_MS 1000
#define CHL_READ_PERIOD_MS 10000

// ADS1115 ADC, on the main I2C bus, reading the chlorophyll board's
// analog output on its AIN0/AIN1 differential input.
static ADS1115 chlSensor(&i2c1);

void setup(void) {
  // enable Vout, 12V by default.
  bristlefin.enableVout();
  // enable 5V out.
  bristlefin.enable5V();
  bristlefin.enable3V();

  if (!chlSensor.init()) {
    printf("ERROR - Failed to initialize ADS1115 chlorophyll sensor!\n");
  }
}

void loop(void) {
  /* USER LOOP CODE GOES HERE */
  // Read the chlorophyll sensor voltage every CHL_READ_PERIOD_MS and log it.
  static uint32_t chlReadTimer = uptimeGetMs();
  if ((uint32_t)uptimeGetMs() - chlReadTimer >= CHL_READ_PERIOD_MS) {
    chlReadTimer = uptimeGetMs();

    float chlVoltage = 0.0f;
    if (chlSensor.readVoltage(chlVoltage)) {
      // Get the RTC if available
      RTCTimeAndDate_t time_and_date = {};
      rtcGet(&time_and_date);
      char rtcTimeBuffer[32];
      rtcPrint(rtcTimeBuffer, &time_and_date);

      // Log the reading to a file, to the spotter_log_console console, and to the printf console.
      spotter_log(0, "chl_data.log", USE_TIMESTAMP, "tick: %llu, rtc: %s, chl_voltage: %.6f\n",
                  uptimeGetMs(), rtcTimeBuffer, chlVoltage);
      spotter_log_console(0, "[chl] | tick: %llu, rtc: %s, chl_voltage: %.6f", uptimeGetMs(),
                          rtcTimeBuffer, chlVoltage);
      printf("[chl] | tick: %llu, rtc: %s, chl_voltage: %.6f\n", uptimeGetMs(), rtcTimeBuffer,
             chlVoltage);
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
