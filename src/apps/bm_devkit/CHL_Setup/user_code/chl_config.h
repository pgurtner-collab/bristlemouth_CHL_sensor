#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Runtime configuration for the chlorophyll node.
 *
 * Every number that might need to change after the enclosure is closed lives
 * here rather than in a #define, because reflashing needs the USB port and the
 * USB port needs the buoy on a bench. Values persist in the mote's user config
 * partition and survive a reboot.
 *
 * From the mote's own console:
 *     cfg usr set chlWipeIntervalMin uint 30
 *     cfg usr save                      (this reboots the mote, which applies it)
 *
 * Or from the Spotter console, addressing the node over Bristlemouth:
 *     bm cfg set <node id> u u chlWipeIntervalMin 30
 *     bm cfg commit <node id> u
 *
 * The `chl set` CLI command wraps the same thing and applies the change without
 * a reboot where that is safe. See docs in the project repo for which is which.
 */

typedef enum {
  CHL_CFG_UINT,
  CHL_CFG_FLOAT,
} chlCfgType_e;

typedef struct {
  const char *key;
  chlCfgType_e type;
  const char *units;
  const char *help;
  union {
    uint32_t u;
    float f;
  } dflt;
  union {
    uint32_t u;
    float f;
  } min;
  union {
    uint32_t u;
    float f;
  } max;
} chlCfgEntry_t;

/*! Live values. Read directly; write only through chlCfgSet(). */
typedef struct {
  uint32_t aggPeriodMs;
  uint32_t samplePeriodMs;
  uint32_t wipeIntervalMin;
  uint32_t wipeSweeps;
  uint32_t parkUs;
  uint32_t sweepUs;
  uint32_t travelMs;
  uint32_t settleMs;
  uint32_t stallMa;
  uint32_t pwrAddr;
  uint32_t adsPga;
  uint32_t rawLog;
  uint32_t wipeOnBoot;
  float calOffsetV;
  float calScale;
} chlConfig_t;

extern chlConfig_t chlCfg;

/*! Load every key from NVM, falling back to the default where unset. */
void chlCfgLoad(void);

/*! Table of every key, for the CLI and for the boot banner. */
const chlCfgEntry_t *chlCfgTable(size_t &count);

/*!
 Set one key by name, range-check it, and optionally persist it.

 \param[in] key    Config key name
 \param[in] valStr Value, parsed according to the key's declared type
 \param[in] persist true to write NVM as well as the live value
 \return true if the key was found, parsed and within range
*/
bool chlCfgSet(const char *key, const char *valStr, bool persist);

/*! Print the whole table with current values. */
void chlCfgPrint(void);
