#include "config.h"

// Four buttons captured with rtl_433 -A on the workbench RTL-SDR. Each is an
// 18-bit fixed codeword (not rolling). Order matches the BTN_* enum.
const Button BUTTONS[] = {
  { "up",     0x7f454 },  // BTN_UP     - retract
  { "down",   0x7f45c },  // BTN_DOWN   - extend
  { "auto",   0x7f480 },  // BTN_AUTO   - web-page button
  { "manual", 0x7f484 },  // BTN_MANUAL - web-page button
};
const uint8_t NUM_BUTTONS = sizeof(BUTTONS) / sizeof(BUTTONS[0]);
