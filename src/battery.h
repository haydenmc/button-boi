// Battery voltage and USB-power sensing (TinyS3 defaults: VBAT divider on
// GPIO10, USB divider on GPIO33). Configured in Kconfig "button-boi → Battery".
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Battery voltage in mV, or 0 if sensing is disabled or failed.
// Takes a few ms (averages several ADC samples).
uint16_t battery_read_mv(void);

// True when USB power is present (always false if not configured).
bool battery_usb_present(void);
