/*
 * `chl` console command - the bench tool for this node.
 *
 * The thing it exists for is setting the wiper endpoints. Those are a property
 * of how the brush and horn ended up clocked on the spline, so they change every
 * time either is disturbed, and there is no way to calculate them - you jog the
 * servo, watch where the brush lands on the optical window, and store the two
 * pulse widths you liked. `chl jog` / `chl setpark` / `chl setsweep` are that
 * loop, and they persist to NVM so the next boot comes up with them.
 */

#include "FreeRTOS.h"
#include "FreeRTOS_CLI.h"
#include "chl_app.h"
#include "chl_config.h"
#include "cli.h"
#include "debug.h"
#include "motor_code.h"
#include "uptime.h"
#include <stdlib.h>
#include <string.h>

static BaseType_t chlCommand(char *writeBuffer, size_t writeBufferLen,
                             const char *commandString);

static const CLI_Command_Definition_t cmdChl = {
    "chl",
    "chl:\n"
    " * chl status              - sensor, wiper and calibration state\n"
    " * chl cfg                 - list every runtime config key\n"
    " * chl set <key> <value>   - set a key and save it to NVM\n"
    " * chl read                - one ADC conversion, raw counts and ug/L\n"
    " * chl scan                - probe the I2C devices this app needs\n"
    " * chl jog <us>            - drive the servo to a pulse width and HOLD\n"
    " * chl nudge <+/-us>       - move relative to the current pulse width\n"
    " * chl park | chl sweep    - drive to a stored endpoint\n"
    " * chl release             - stop driving the servo (do this after jogging)\n"
    " * chl setpark [us]        - store the current (or given) pulse as park\n"
    " * chl setsweep [us]       - store the current (or given) pulse as sweep\n"
    " * chl wipe [sweeps]       - run a cleaning cycle now\n"
    " * chl tx                  - close the averaging window and transmit now\n",
    chlCommand,
    -1};

void chlCliInit(void) { FreeRTOS_CLIRegisterCommand(&cmdChl); }

/*! Copy a CLI parameter into a NUL-terminated buffer. */
static bool paramCopy(const char *commandString, UBaseType_t index, char *out, size_t outLen) {
  BaseType_t len = 0;
  const char *p = FreeRTOS_CLIGetParameter(commandString, index, &len);
  if (p == nullptr || len <= 0 || (size_t)len >= outLen) {
    return false;
  }
  memcpy(out, p, (size_t)len);
  out[len] = '\0';
  return true;
}

/*! Store one endpoint and persist it. */
static void storeEndpoint(const char *key, uint16_t pulse_us) {
  char valBuf[16];
  snprintf(valBuf, sizeof(valBuf), "%u", (unsigned)pulse_us);
  if (chlCfgSet(key, valBuf, true)) {
    chlAppApplyConfig();
    printf("Stored %s = %u us. It survives a reboot.\n", key, (unsigned)pulse_us);
  }
}

static BaseType_t chlCommand(char *writeBuffer, size_t writeBufferLen,
                             const char *commandString) {
  (void)writeBuffer;
  (void)writeBufferLen;

  char sub[24] = {};
  if (!paramCopy(commandString, 1, sub, sizeof(sub))) {
    printf("%s", cmdChl.pcHelpString);
    return pdFALSE;
  }

  if (!strcmp(sub, "status")) {
    chlAppStatus();

  } else if (!strcmp(sub, "cfg")) {
    chlCfgPrint();

  } else if (!strcmp(sub, "set")) {
    char key[36] = {};
    char val[24] = {};
    if (!paramCopy(commandString, 2, key, sizeof(key)) ||
        !paramCopy(commandString, 3, val, sizeof(val))) {
      printf("usage: chl set <key> <value>\n");
      return pdFALSE;
    }
    if (chlCfgSet(key, val, true)) {
      chlAppApplyConfig();
      printf("NOTE: chlAggPeriodMs and chlSamplePeriodMs size a buffer at boot, so "
             "those two need a `reset` to take effect. Everything else is live.\n");
    }

  } else if (!strcmp(sub, "read")) {
    chlReading_t r = chlAppReadOnce();
    if (!r.ok) {
      printf("ADC read FAILED\n");
      return pdFALSE;
    }
    printf("counts: %d, volts: %.6f, chl: %.4f ug/L%s\n", r.counts, (double)r.volts,
           (double)r.ugl,
           r.saturated ? "   *** at the ADC rail - the reading is clipped, not high ***" : "");

  } else if (!strcmp(sub, "scan")) {
    chlAppScanI2C();

  } else if (!strcmp(sub, "jog")) {
    char val[16] = {};
    if (!paramCopy(commandString, 2, val, sizeof(val))) {
      printf("usage: chl jog <pulse width in us, %u-%u>\n", (unsigned)MOTOR_MIN_PULSE_US,
             (unsigned)MOTOR_MAX_PULSE_US);
      return pdFALSE;
    }
    const long us = strtol(val, nullptr, 0);
    if (us < MOTOR_MIN_PULSE_US || us > MOTOR_MAX_PULSE_US) {
      printf("ERR - %ld us is outside the servo's %u-%u us range.\n", us,
             (unsigned)MOTOR_MIN_PULSE_US, (unsigned)MOTOR_MAX_PULSE_US);
      return pdFALSE;
    }
    if (motorIsBusy()) {
      printf("ERR - a cleaning cycle is running; wait for it to finish.\n");
      return pdFALSE;
    }
    if (motorSetPulseUs((uint16_t)us)) {
      printf("servo -> %ld us (HOLDING torque - `chl release` when you are done)\n", us);
    } else {
      printf("ERR - servo command failed. Is the PCA9685 initialized? Try `chl scan`.\n");
    }

  } else if (!strcmp(sub, "nudge")) {
    char val[16] = {};
    if (!paramCopy(commandString, 2, val, sizeof(val))) {
      printf("usage: chl nudge <+/- microseconds>\n");
      return pdFALSE;
    }
    const long delta = strtol(val, nullptr, 0);
    long target = (long)motorGetPulseUs() + delta;
    if (target < MOTOR_MIN_PULSE_US) target = MOTOR_MIN_PULSE_US;
    if (target > MOTOR_MAX_PULSE_US) target = MOTOR_MAX_PULSE_US;
    if (motorSetPulseUs((uint16_t)target)) {
      printf("servo -> %ld us\n", target);
    }

  } else if (!strcmp(sub, "park")) {
    if (motorSetPulseUs(motorGetConfig().park_us)) {
      printf("servo -> park, %u us\n", motorGetConfig().park_us);
    }

  } else if (!strcmp(sub, "sweep")) {
    if (motorSetPulseUs(motorGetConfig().sweep_us)) {
      printf("servo -> sweep, %u us\n", motorGetConfig().sweep_us);
    }

  } else if (!strcmp(sub, "release")) {
    if (motorRelease()) {
      printf("servo released - no torque, it will back-drive freely.\n");
    }

  } else if (!strcmp(sub, "setpark") || !strcmp(sub, "setsweep")) {
    const bool isPark = (sub[3] == 'p');
    char val[16] = {};
    uint16_t pulse = motorGetPulseUs();
    if (paramCopy(commandString, 2, val, sizeof(val))) {
      const long us = strtol(val, nullptr, 0);
      if (us < MOTOR_MIN_PULSE_US || us > MOTOR_MAX_PULSE_US) {
        printf("ERR - %ld us is outside the servo's %u-%u us range.\n", us,
               (unsigned)MOTOR_MIN_PULSE_US, (unsigned)MOTOR_MAX_PULSE_US);
        return pdFALSE;
      }
      pulse = (uint16_t)us;
    }
    storeEndpoint(isPark ? "chlParkUs" : "chlSweepUs", pulse);

  } else if (!strcmp(sub, "wipe")) {
    char val[8] = {};
    uint8_t sweeps = 0; // 0 = use the configured count
    if (paramCopy(commandString, 2, val, sizeof(val))) {
      const long n = strtol(val, nullptr, 0);
      if (n < 1 || n > 20) {
        printf("ERR - sweeps must be 1..20.\n");
        return pdFALSE;
      }
      sweeps = (uint8_t)n;
    }
    if (!motorStartCleaningCycle(sweeps)) {
      printf("ERR - could not start a cycle.\n");
    }

  } else if (!strcmp(sub, "tx")) {
    printf("Closing the window early and transmitting...\n");
    chlAppReportNow();

  } else {
    printf("unknown subcommand '%s'\n%s", sub, cmdChl.pcHelpString);
  }

  return pdFALSE;
}
