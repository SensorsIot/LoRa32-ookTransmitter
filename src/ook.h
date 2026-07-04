#pragma once
#include "config.h"

// SX1278 OOK carrier keying (FR-1.x). All functions run on the TX task only.

// Initialize the radio in FSK/OOK direct mode. Returns false on failure (FR-3.1).
bool ookInit();

// True once ookInit() has succeeded.
bool ookReady();

// Transmit BUTTONS[idx]'s codeword `transmissions` times; each transmission is
// REPEATS words with the shared timing template, MSB-first (FR-1.2/1.4/1.5).
void ookSend(uint8_t idx, uint8_t transmissions);
